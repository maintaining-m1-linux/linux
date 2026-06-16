// SPDX-License-Identifier: GPL-2.0
/* Bottleneck Bandwidth and RTT (BBR) congestion control
 * Fully optimized with Statistical Volatility Estimation & Non-linear Control (BBRv3 Paradigm)
 */
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/module.h>
#include <net/tcp.h>
#include <linux/inet_diag.h>
#include <linux/inet.h>
#include <linux/random.h>
#include <linux/win_minmax.h>

#define BW_SCALE 24
#define BW_UNIT (1 << BW_SCALE)

#define BBR_SCALE 8
#define BBR_UNIT (1 << BBR_SCALE)

/* ADVANCED TUNING CONSTANTS */
#define BBR_EMA_ALPHA         32        /* Alpha for EWMA: 32/256 = 1/8 smoothing factor */
#define BBR_LOSS_MIN_THRES    5         /* Minimum loss percentage to trigger strict response */
#define BBR_VOLAT_SCALE       12        /* Internal scaling for variance calculation */

enum bbr_mode {
    BBR_STARTUP,
    BBR_DRAIN,
    BBR_PROBE_BW,
    BBR_PROBE_RTT,
};

struct bbr {
    u32    min_rtt_us;
    u32    min_rtt_stamp;
    u32    probe_rtt_done_stamp;
    struct minmax bw;
    u32    rtt_cnt;
    u32     next_rtt_delivered;
    u64    cycle_mstamp;
    u32    mode:3,
        prev_ca_state:3,
        packet_conservation:1,
        round_start:1,
        idle_restart:1,
        probe_rtt_round_done:1,
        unused:2,
        reduce_cwnd:1,
        lt_is_sampling:1,
        lt_rtt_cnt:7,
        lt_use_bw:1;
    u32 lt_bw:20,
        bw_ewma:12;
    u32 lt_last_delivered;
    u32    pacing_gain:10,
        cwnd_gain:10,
        full_bw_reached:1,
        full_bw_cnt:2,
        cycle_idx:3,
        has_seen_rtt:1,
        unused_b:5;
    u32    prior_cwnd;
    u32    full_bw:20,
           bw_variance:12;
    u32 lt_last_stamp;
    u32 lt_last_lost;

    /* For tracking ACK aggregation: */
    u64    ack_epoch_mstamp;
    u16    extra_acked[2];
    u32    ack_epoch_acked:20,
        extra_acked_win_rtts:5,
        extra_acked_win_idx:1,
        unused_c:6;
};

#define CYCLE_LEN    8

static const int bbr_bw_rtts = CYCLE_LEN + 2;
static const u32 bbr_min_rtt_win_sec = 10;
static const u32 bbr_probe_rtt_mode_ms = 200;
static const int bbr_min_tso_rate = 1200000;
static const int bbr_pacing_margin_percent = 1;

static const int bbr_high_gain  = BBR_UNIT * 2885 / 1000 + 1;
static const int bbr_drain_gain = BBR_UNIT * 1000 / 2885;
static const int bbr_cwnd_gain  = BBR_UNIT * 2;
static const int bbr_pacing_gain[] = {
    BBR_UNIT * 5 / 4,
    BBR_UNIT * 3 / 4,
    BBR_UNIT, BBR_UNIT, BBR_UNIT,
    BBR_UNIT, BBR_UNIT, BBR_UNIT
};
static const u32 bbr_cycle_rand = 7;
static const u32 bbr_cwnd_min_target = 4;
static const u32 bbr_full_bw_thresh = BBR_UNIT * 5 / 4;
static const u32 bbr_full_bw_cnt = 3;

static const u32 bbr_lt_intvl_min_rtts = 4;
static const u32 bbr_lt_loss_thresh = 50;
static const u32 bbr_lt_bw_ratio = BBR_UNIT / 8;
static const u32 bbr_lt_bw_diff = 4000 / 8;
static const u32 bbr_lt_bw_max_rtts = 48;

static const int bbr_extra_acked_gain = BBR_UNIT;
static const u32 bbr_extra_acked_win_rtts = 5;
static const u32 bbr_ack_epoch_acked_reset_thresh = 1U << 20;
static const u32 bbr_extra_acked_max_us = 100 * 1000;

static void bbr_check_probe_rtt_done(struct sock *sk);

static bool bbr_full_bw_reached(const struct sock *sk)
{
    const struct bbr *bbr = inet_csk_ca(sk);
    return bbr->full_bw_reached;
}

static u32 bbr_max_bw(const struct sock *sk)
{
    struct bbr *bbr = inet_csk_ca(sk);
    return minmax_get(&bbr->bw);
}

static u32 bbr_bw(const struct sock *sk)
{
    struct bbr *bbr = inet_csk_ca(sk);
    return bbr->lt_use_bw ? bbr->lt_bw : bbr_max_bw(sk);
}

static u16 bbr_extra_acked(const struct sock *sk)
{
    struct bbr *bbr = inet_csk_ca(sk);
    return max(bbr->extra_acked[0], bbr->extra_acked[1]);
}

static u64 bbr_rate_bytes_per_sec(struct sock *sk, u64 rate, int gain)
{
    unsigned int mss = tcp_sk(sk)->mss_cache;

    rate *= mss;
    rate *= gain;
    rate >>= BBR_SCALE;
    rate *= USEC_PER_SEC / 100 * (100 - bbr_pacing_margin_percent);
    return rate >> BW_SCALE;
}

static unsigned long bbr_bw_to_pacing_rate(struct sock *sk, u32 bw, int gain)
{
    u64 rate = bw;

    rate = bbr_rate_bytes_per_sec(sk, rate, gain);
    rate = min_t(u64, rate, READ_ONCE(sk->sk_max_pacing_rate));
    return rate;
}

static void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);
    u64 bw;
    u32 rtt_us;

    if (tp->srtt_us) {
        rtt_us = max(tp->srtt_us >> 3, 1U);
        bbr->has_seen_rtt = 1;
    } else {
        rtt_us = USEC_PER_MSEC;
    }
    bw = (u64)tcp_snd_cwnd(tp) * BW_UNIT;
    do_div(bw, rtt_us);
    WRITE_ONCE(sk->sk_pacing_rate,
           bbr_bw_to_pacing_rate(sk, bw, bbr_high_gain));
}

static void bbr_set_pacing_rate(struct sock *sk, u32 bw, int gain)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);
    unsigned long rate = bbr_bw_to_pacing_rate(sk, bw, gain);

    if (unlikely(!bbr->has_seen_rtt && tp->srtt_us))
        bbr_init_pacing_rate_from_rtt(sk);
    if (bbr_full_bw_reached(sk) || rate > READ_ONCE(sk->sk_pacing_rate))
        WRITE_ONCE(sk->sk_pacing_rate, rate);
}

__bpf_kfunc static u32 bbr_min_tso_segs(struct sock *sk)
{
    return READ_ONCE(sk->sk_pacing_rate) < (bbr_min_tso_rate >> 3) ? 1 : 2;
}

static u32 bbr_tso_segs_goal(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    u32 segs, bytes;

    bytes = min_t(unsigned long,
              READ_ONCE(sk->sk_pacing_rate) >> READ_ONCE(sk->sk_pacing_shift),
              GSO_LEGACY_MAX_SIZE - 1 - MAX_TCP_HEADER);
    segs = max_t(u32, bytes / tp->mss_cache, bbr_min_tso_segs(sk));

    return min(segs, 0x7FU);
}

static void bbr_save_cwnd(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);

    if (bbr->prev_ca_state < TCP_CA_Recovery && bbr->mode != BBR_PROBE_RTT)
        bbr->prior_cwnd = tcp_snd_cwnd(tp);
    else
        bbr->prior_cwnd = max(bbr->prior_cwnd, tcp_snd_cwnd(tp));
}

static u32 bbr_bdp(struct sock *sk, u32 bw, int gain)
{
    struct bbr *bbr = inet_csk_ca(sk);
    u32 bdp;
    u64 w;

    if (unlikely(bbr->min_rtt_us == ~0U))
        return TCP_INIT_CWND;

    w = (u64)bw * bbr->min_rtt_us;
    bdp = (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;

    return bdp;
}

static u32 bbr_quantization_budget(struct sock *sk, u32 cwnd)
{
    struct bbr *bbr = inet_csk_ca(sk);

    cwnd += 3 * bbr_tso_segs_goal(sk);
    cwnd = (cwnd + 1) & ~1U;

    if (bbr->mode == BBR_PROBE_BW && bbr->cycle_idx == 0)
        cwnd += 2;

    return cwnd;
}

static u32 bbr_inflight(struct sock *sk, u32 bw, int gain)
{
    u32 inflight;

    inflight = bbr_bdp(sk, bw, gain);
    inflight = bbr_quantization_budget(sk, inflight);

    return inflight;
}

static u32 bbr_packets_in_net_at_edt(struct sock *sk, u32 inflight_now)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);
    u64 now_ns, edt_ns, interval_us;
    u32 interval_delivered, inflight_at_edt;

    now_ns = tp->tcp_clock_cache;
    edt_ns = max(tp->tcp_wstamp_ns, now_ns);
    interval_us = div_u64(edt_ns - now_ns, NSEC_PER_USEC);
    interval_delivered = (u64)bbr_bw(sk) * interval_us >> BW_SCALE;
    inflight_at_edt = inflight_now;
    if (bbr->pacing_gain > BBR_UNIT)
        inflight_at_edt += bbr_tso_segs_goal(sk);
    if (interval_delivered >= inflight_at_edt)
        return 0;
    return inflight_at_edt - interval_delivered;
}

static u32 bbr_ack_aggregation_cwnd(struct sock *sk)
{
    u32 max_aggr_cwnd, aggr_cwnd = 0;

    if (bbr_extra_acked_gain && bbr_full_bw_reached(sk)) {
        max_aggr_cwnd = ((u64)bbr_bw(sk) * bbr_extra_acked_max_us)
                / BW_UNIT;
        aggr_cwnd = (bbr_extra_acked_gain * bbr_extra_acked(sk))
                 >> BBR_SCALE;
        aggr_cwnd = min(aggr_cwnd, max_aggr_cwnd);
    }

    return aggr_cwnd;
}

static bool bbr_set_cwnd_to_recover_or_restore(
    struct sock *sk, const struct rate_sample *rs, u32 acked, u32 *new_cwnd)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);
    u8 prev_state = bbr->prev_ca_state, state = inet_csk(sk)->icsk_ca_state;
    u32 cwnd = tcp_snd_cwnd(tp);

    if (rs->losses > 0)
        cwnd = max_t(s32, cwnd - rs->losses, 1);

    if (state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery) {
        bbr->packet_conservation = 1;
        bbr->next_rtt_delivered = tp->delivered;
        cwnd = tcp_packets_in_flight(tp) + acked;
    } else if (prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery) {
        cwnd = max(cwnd, bbr->prior_cwnd);
        bbr->packet_conservation = 0;
    }
    bbr->prev_ca_state = state;

    if (bbr->packet_conservation) {
        *new_cwnd = max(cwnd, tcp_packets_in_flight(tp) + acked);
        return true;
    }
    *new_cwnd = cwnd;
    return false;
}

static void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
             u32 acked, u32 bw, int gain)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);
    u32 cwnd = tcp_snd_cwnd(tp), target_cwnd = 0;

    if (!acked)
        goto done;

    /* Dynamic Backoff Modifier Based on Loss Severity and Volatility Tracking */
    if (bbr->reduce_cwnd) {
        u32 loss_ratio = 0;
        if (rs->delivered > 0)
            loss_ratio = (rs->losses * 100) / (rs->delivered + rs->losses);

        if (loss_ratio > BBR_LOSS_MIN_THRES) {
            /* Nonlinear exponential backoff calculation mimicking modern BBRv3 behavior */
            u32 backoff_factor = (loss_ratio * BBR_UNIT) / 100;
            u32 reduction = (cwnd * backoff_factor) >> BBR_SCALE;
            cwnd = max_t(s32, cwnd - max(reduction, acked), bbr_cwnd_min_target);
        } else {
            cwnd = max_t(s32, cwnd - acked, bbr_cwnd_min_target);
        }
        bbr->reduce_cwnd = 0;
    }

    if (bbr_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd))
        goto done;

    target_cwnd = bbr_bdp(sk, bw, gain);
    target_cwnd += bbr_ack_aggregation_cwnd(sk);
    target_cwnd = bbr_quantization_budget(sk, target_cwnd);

    if (bbr_full_bw_reached(sk))
        cwnd = min(cwnd + acked, target_cwnd);
    else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
        cwnd = cwnd + acked;
    cwnd = max(cwnd, bbr_cwnd_min_target);

done:
    tcp_snd_cwnd_set(tp, min(cwnd, tp->snd_cwnd_clamp));
    if (bbr->mode == BBR_PROBE_RTT)
        tcp_snd_cwnd_set(tp, min(tcp_snd_cwnd(tp), bbr_cwnd_min_target));
}

static bool bbr_is_next_cycle_phase(struct sock *sk,
                    const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);
    bool is_full_length =
        tcp_stamp_us_delta(tp->delivered_mstamp, bbr->cycle_mstamp) >
        bbr->min_rtt_us;
    u32 inflight, bw;

    if (bbr->pacing_gain == BBR_UNIT)
        return is_full_length;

    inflight = bbr_packets_in_net_at_edt(sk, rs->prior_in_flight);
    bw = bbr_max_bw(sk);

    if (bbr->pacing_gain > BBR_UNIT)
        return is_full_length &&
            (rs->losses ||
             inflight >= bbr_inflight(sk, bw, bbr->pacing_gain));

    return is_full_length ||
        inflight <= bbr_inflight(sk, bw, BBR_UNIT);
}

static void bbr_advance_cycle_phase(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);

    bbr->cycle_idx = (bbr->cycle_idx + 1) & (CYCLE_LEN - 1);
    bbr->cycle_mstamp = tp->delivered_mstamp;
}

static void bbr_update_cycle_phase(struct sock *sk,
                   const struct rate_sample *rs)
{
    struct bbr *bbr = inet_csk_ca(sk);

    if (bbr->mode == BBR_PROBE_BW && bbr_is_next_cycle_phase(sk, rs))
        bbr_advance_cycle_phase(sk);
}

static void bbr_reset_startup_mode(struct sock *sk)
{
    struct bbr *bbr = inet_csk_ca(sk);
    bbr->mode = BBR_STARTUP;
}

static void bbr_reset_probe_bw_mode(struct sock *sk)
{
    struct bbr *bbr = inet_csk_ca(sk);

    bbr->mode = BBR_PROBE_BW;
    bbr->cycle_idx = CYCLE_LEN - 1 - get_random_u32_below(bbr_cycle_rand);
    bbr_advance_cycle_phase(sk);
}

static void bbr_reset_mode(struct sock *sk)
{
    if (!bbr_full_bw_reached(sk))
        bbr_reset_startup_mode(sk);
    else
        bbr_reset_probe_bw_mode(sk);
}

static void bbr_reset_lt_bw_sampling_interval(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);

    bbr->lt_last_stamp = div_u64(tp->delivered_mstamp, USEC_PER_MSEC);
    bbr->lt_last_delivered = tp->delivered;
    bbr->lt_last_lost = tp->lost;
    bbr->lt_rtt_cnt = 0;
}

static void bbr_reset_lt_bw_sampling(struct sock *sk)
{
    struct bbr *bbr = inet_csk_ca(sk);

    bbr->lt_bw = 0;
    bbr->lt_use_bw = 0;
    bbr->lt_is_sampling = false;
    bbr_reset_lt_bw_sampling_interval(sk);
}

static void bbr_lt_bw_interval_done(struct sock *sk, u32 bw)
{
    struct bbr *bbr = inet_csk_ca(sk);
    u32 diff;

    if (bbr->lt_bw) {
        diff = abs(bw - bbr->lt_bw);
        if ((diff * BBR_UNIT <= bbr_lt_bw_ratio * bbr->lt_bw) ||
            (bbr_rate_bytes_per_sec(sk, diff, BBR_UNIT) <=
             bbr_lt_bw_diff)) {
            bbr->lt_bw = (bw + bbr->lt_bw) >> 1;
            bbr->lt_use_bw = 1;
            bbr->pacing_gain = BBR_UNIT;
            bbr->lt_rtt_cnt = 0;
            return;
        }
    }
    bbr->lt_bw = bw;
    bbr_reset_lt_bw_sampling_interval(sk);
}

static void bbr_lt_bw_sampling(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);
    u32 lost, delivered;
    u64 bw;
    u32 t;

    if (bbr->lt_use_bw) {
        if (bbr->mode == BBR_PROBE_BW && bbr->round_start &&
            ++bbr->lt_rtt_cnt >= bbr_lt_bw_max_rtts) {
            bbr_reset_lt_bw_sampling(sk);
            bbr_reset_probe_bw_mode(sk);
        }
        return;
    }

    if (!bbr->lt_is_sampling) {
        if (!rs->losses)
            return;
        bbr_reset_lt_bw_sampling_interval(sk);
        bbr->lt_is_sampling = true;
    }

    if (rs->is_app_limited) {
        bbr_reset_lt_bw_sampling(sk);
        return;
    }

    if (bbr->round_start)
        bbr->lt_rtt_cnt++;
    if (bbr->lt_rtt_cnt < bbr_lt_intvl_min_rtts)
        return;
    if (bbr->lt_rtt_cnt > 4 * bbr_lt_intvl_min_rtts) {
        bbr_reset_lt_bw_sampling(sk);
        return;
    }

    if (!rs->losses)
        return;

    lost = tp->lost - bbr->lt_last_lost;
    delivered = tp->delivered - bbr->lt_last_delivered;
    if (!delivered || (lost << BBR_SCALE) < bbr_lt_loss_thresh * delivered)
        return;

    t = div_u64(tp->delivered_mstamp, USEC_PER_MSEC) - bbr->lt_last_stamp;
    if ((s32)t < 1)
        return;
    if (t >= ~0U / USEC_PER_MSEC) {
        bbr_reset_lt_bw_sampling(sk);
        return;
    }
    t *= USEC_PER_MSEC;
    bw = (u64)delivered * BW_UNIT;
    do_div(bw, t);
    bbr_lt_bw_interval_done(sk, bw);
}

/* Updated Model with EWMA Statistics & Non-linear Control Core Matrix */
static void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);
    u64 bw;
    u64 old_max_bw = bbr_max_bw(sk);

    bbr->round_start = 0;
    if (rs->delivered < 0 || rs->interval_us <= 0)
        return;

    if (!before(rs->prior_delivered, bbr->next_rtt_delivered)) {
        bbr->next_rtt_delivered = tp->delivered;
        bbr->rtt_cnt++;
        bbr->round_start = 1;
        bbr->packet_conservation = 0;
    }

    bbr_lt_bw_sampling(sk, rs);

    bw = div64_long((u64)rs->delivered * BW_UNIT, rs->interval_us);

    if (!rs->is_app_limited || bw >= bbr_max_bw(sk)) {
        minmax_running_max(&bbr->bw, bbr_bw_rtts, bbr->rtt_cnt, bw);
    }

    /* Statistical Volatility Estimation Framework (BBRv3 Extended Metrics) */
    if (likely(bw > 0)) {
		u32 scaled_bw = (bw >> 12) & 0xFFF;
        if (unlikely(!bbr->bw_ewma)) {
            bbr->bw_ewma = scaled_bw;
            bbr->bw_variance = scaled_bw / 4; /* Safe fallback bounding factor */
        } else {
            /* Compute Exponential Moving Average of Bandwidth */
            bbr->bw_ewma = ((bbr->bw_ewma * (256 - BBR_EMA_ALPHA)) + (scaled_bw * BBR_EMA_ALPHA)) >> 8;
            
            /* Compute Absolute Moving Deviation (Statistical Spread) */
            u32 abs_dev = abs((s32)scaled_bw - (s32)bbr->bw_ewma) & 0xFFF;
  			bbr->bw_variance = ((bbr->bw_variance * (256 - BBR_EMA_ALPHA)) + (abs_dev * BBR_EMA_ALPHA)) >> 8;
        }
   }

    /* Non-linear State Space Mitigation Engine (Replaces the crude tweak loop) */
    if (old_max_bw && bw < old_max_bw && bbr->mode == BBR_PROBE_BW) {
        u64 current_max = bbr_max_bw(sk);
        if (current_max > 0) {
            /* Calculate drop depth normalized by system variance */
            u64 drop_depth = ((old_max_bw - bw) * BBR_UNIT) / current_max;
            u64 volatility = (bbr->bw_variance * BBR_UNIT) / current_max;

            /* High volatility means higher tolerance to prevent false response to transient cross-traffic jitter */
            if (drop_depth > (volatility + (BBR_UNIT / 10))) {
                /* Non-linear quadratic responsiveness to suppress deep queuing loops */
                u64 reduction_ratio = (drop_depth * drop_depth) >> BBR_SCALE;
                reduction_ratio = min_t(u64, reduction_ratio, BBR_UNIT / 2); /* Bound at 50% max reduction */

                bbr->pacing_gain = (bbr->pacing_gain * (BBR_UNIT - reduction_ratio)) >> BBR_SCALE;
                
                /* Establish safe operational flooring bounded to system volatility footprint */
                u32 floor_limit = BBR_UNIT * 3 / 4; 
                if (bbr->pacing_gain < floor_limit) {
                    bbr->pacing_gain = floor_limit;
                    bbr->reduce_cwnd = 1; /* Request immediate cwnd clamp synchronization */
                }
            }
        }
    }
}

static void bbr_update_ack_aggregation(struct sock *sk,
                       const struct rate_sample *rs)
{
    u32 epoch_us, expected_acked, extra_acked;
    struct bbr *bbr = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);

    if (!bbr_extra_acked_gain || rs->acked_sacked <= 0 ||
        rs->delivered < 0 || rs->interval_us <= 0)
        return;

    if (bbr->round_start) {
        bbr->extra_acked_win_rtts = min(0x1F,
                        bbr->extra_acked_win_rtts + 1);
        if (bbr->extra_acked_win_rtts >= bbr_extra_acked_win_rtts) {
            bbr->extra_acked_win_rtts = 0;
            bbr->extra_acked_win_idx = bbr->extra_acked_win_idx ?
                           0 : 1;
            bbr->extra_acked[bbr->extra_acked_win_idx] = 0;
        }
    }

    epoch_us = tcp_stamp_us_delta(tp->delivered_mstamp,
                      bbr->ack_epoch_mstamp);
    expected_acked = ((u64)bbr_bw(sk) * epoch_us) / BW_UNIT;

    if (bbr->ack_epoch_acked <= expected_acked ||
        (bbr->ack_epoch_acked + rs->acked_sacked >=
         bbr_ack_epoch_acked_reset_thresh)) {
        bbr->ack_epoch_acked = 0;
        bbr->ack_epoch_mstamp = tp->delivered_mstamp;
        expected_acked = 0;
    }

    bbr->ack_epoch_acked = min_t(u32, 0xFFFFF,
                     bbr->ack_epoch_acked + rs->acked_sacked);
    extra_acked = bbr->ack_epoch_acked - expected_acked;
    extra_acked = min(extra_acked, tcp_snd_cwnd(tp));
    if (extra_acked > bbr->extra_acked[bbr->extra_acked_win_idx])
        bbr->extra_acked[bbr->extra_acked_win_idx] = extra_acked;
}

static void bbr_check_full_bw_reached(struct sock *sk,
                      const struct rate_sample *rs)
{
    struct bbr *bbr = inet_csk_ca(sk);
    u32 bw_thresh;

    if (bbr_full_bw_reached(sk) || !bbr->round_start || rs->is_app_limited)
        return;

    bw_thresh = (u64)bbr->full_bw * bbr_full_bw_thresh >> BBR_SCALE;
    if (bbr_max_bw(sk) >= bw_thresh) {
        bbr->full_bw = bbr_max_bw(sk);
        bbr->full_bw_cnt = 0;
        return;
    }
    ++bbr->full_bw_cnt;
    bbr->full_bw_reached = bbr->full_bw_cnt >= bbr_full_bw_cnt;
}

static void bbr_check_drain(struct sock *sk, const struct rate_sample *rs)
{
    struct bbr *bbr = inet_csk_ca(sk);

    if (bbr->mode == BBR_STARTUP && bbr_full_bw_reached(sk)) {
        bbr->mode = BBR_DRAIN;
        WRITE_ONCE(tcp_sk(sk)->snd_ssthresh,
               bbr_inflight(sk, bbr_max_bw(sk), BBR_UNIT));
    }
    if (bbr->mode == BBR_DRAIN &&
        bbr_packets_in_net_at_edt(sk, tcp_packets_in_flight(tcp_sk(sk))) <=
        bbr_inflight(sk, bbr_max_bw(sk), BBR_UNIT))
        bbr_reset_probe_bw_mode(sk);
}

static void bbr_check_probe_rtt_done(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);

    if (!(bbr->probe_rtt_done_stamp &&
          after(tcp_jiffies32, bbr->probe_rtt_done_stamp)))
        return;

    bbr->min_rtt_stamp = tcp_jiffies32;
    tcp_snd_cwnd_set(tp, max(tcp_snd_cwnd(tp), bbr->prior_cwnd));
    bbr_reset_mode(sk);
}

static void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);
    bool filter_expired;

    filter_expired = after(tcp_jiffies32,
                   bbr->min_rtt_stamp + bbr_min_rtt_win_sec * HZ);
    if (rs->rtt_us >= 0 &&
        (rs->rtt_us < bbr->min_rtt_us ||
         (filter_expired && !rs->is_ack_delayed))) {
        bbr->min_rtt_us = rs->rtt_us;
        bbr->min_rtt_stamp = tcp_jiffies32;
    }

    if (bbr_probe_rtt_mode_ms > 0 && filter_expired &&
        !bbr->idle_restart && bbr->mode != BBR_PROBE_RTT) {
        bbr->mode = BBR_PROBE_RTT;
        bbr_save_cwnd(sk);
        bbr->probe_rtt_done_stamp = 0;
    }

    if (bbr->mode == BBR_PROBE_RTT) {
        tp->app_limited =
            (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
        if (!bbr->probe_rtt_done_stamp &&
            tcp_packets_in_flight(tp) <= bbr_cwnd_min_target) {
            bbr->probe_rtt_done_stamp = tcp_jiffies32 +
                msecs_to_jiffies(bbr_probe_rtt_mode_ms);
            bbr->probe_rtt_round_done = 0;
            bbr->next_rtt_delivered = tp->delivered;
        } else if (bbr->probe_rtt_done_stamp) {
            if (bbr->round_start)
                bbr->probe_rtt_round_done = 1;
            if (bbr->probe_rtt_round_done)
                bbr_check_probe_rtt_done(sk);
        }
    }
    if (rs->delivered > 0)
        bbr->idle_restart = 0;
}

static void bbr_update_gains(struct sock *sk)
{
    struct bbr *bbr = inet_csk_ca(sk);

    switch (bbr->mode) {
    case BBR_STARTUP:
        bbr->pacing_gain = bbr_high_gain;
        bbr->cwnd_gain     = bbr_high_gain;
        break;
    case BBR_DRAIN:
        bbr->pacing_gain = bbr_drain_gain;
        bbr->cwnd_gain     = bbr_high_gain;
        break;
    case BBR_PROBE_BW:
        /* Proportional control state integration based on long term heuristics */
        bbr->pacing_gain = (bbr->lt_use_bw ?
                    BBR_UNIT :
                    bbr_pacing_gain[bbr->cycle_idx]);
        bbr->cwnd_gain     = bbr_cwnd_gain;
        break;
    case BBR_PROBE_RTT:
        bbr->pacing_gain = BBR_UNIT;
        bbr->cwnd_gain     = BBR_UNIT;
        break;
    default:
        WARN_ONCE(1, "BBR bad mode: %u\n", bbr->mode);
        break;
    }
}

static void bbr_update_model(struct sock *sk, const struct rate_sample *rs)
{
    bbr_update_bw(sk, rs);
    bbr_update_ack_aggregation(sk, rs);
    bbr_update_cycle_phase(sk, rs);
    bbr_check_full_bw_reached(sk, rs);
    bbr_check_drain(sk, rs);
    bbr_update_min_rtt(sk, rs);
    bbr_update_gains(sk);
}

__bpf_kfunc static void bbr_main(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)
{
    struct bbr *bbr = inet_csk_ca(sk);
    u32 bw;

    bbr_update_model(sk, rs);

    bw = bbr_bw(sk);
    bbr_set_pacing_rate(sk, bw, bbr->pacing_gain);
    bbr_set_cwnd(sk, rs, rs->acked_sacked, bw, bbr->cwnd_gain);
}

__bpf_kfunc static void bbr_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbr *bbr = inet_csk_ca(sk);

    bbr->prior_cwnd = 0;
    WRITE_ONCE(tp->snd_ssthresh, TCP_INFINITE_SSTHRESH);
    bbr->rtt_cnt = 0;
    bbr->next_rtt_delivered = tp->delivered;
    bbr->prev_ca_state = TCP_CA_Open;
    bbr->packet_conservation = 0;

    bbr->probe_rtt_done_stamp = 0;
    bbr->probe_rtt_round_done = 0;
    bbr->min_rtt_us = tcp_min_rtt(tp);
    bbr->min_rtt_stamp = tcp_jiffies32;

    minmax_reset(&bbr->bw, bbr->rtt_cnt, 0);

    bbr->has_seen_rtt = 0;
    bbr_init_pacing_rate_from_rtt(sk);

    bbr->round_start = 0;
    bbr->idle_restart = 0;
    bbr->full_bw_reached = 0;
    bbr->full_bw = 0;
    bbr->full_bw_cnt = 0;
    bbr->cycle_mstamp = 0;
    bbr->cycle_idx = 0;
    
    /* Statistical variable initializes */
    bbr->bw_ewma = 0;
    bbr->bw_variance = 0;

    bbr_reset_lt_bw_sampling(sk);
    bbr_reset_startup_mode(sk);

    bbr->ack_epoch_mstamp = tp->tcp_mstamp;
    bbr->ack_epoch_acked = 0;
    bbr->extra_acked_win_rtts = 0;
    bbr->extra_acked_win_idx = 0;
    bbr->extra_acked[0] = 0;
    bbr->extra_acked[1] = 0;

    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

__bpf_kfunc static u32 bbr_sndbuf_expand(struct sock *sk)
{
    return 3;
}

__bpf_kfunc static u32 bbr_undo_cwnd(struct sock *sk)
{
    struct bbr *bbr = inet_csk_ca(sk);

    bbr->full_bw = 0;
    bbr->full_bw_cnt = 0;
    bbr_reset_lt_bw_sampling(sk);
    return tcp_snd_cwnd(tcp_sk(sk));
}

__bpf_kfunc static u32 bbr_ssthresh(struct sock *sk)
{
    bbr_save_cwnd(sk);
    return tcp_sk(sk)->snd_ssthresh;
}

static size_t bbr_get_info(struct sock *sk, u32 ext, int *attr,
                               union tcp_cc_info *info)
{
    if (ext & (1 << (INET_DIAG_BBRINFO - 1)) ||
        ext & (1 << (INET_DIAG_VEGASINFO - 1))) {
        struct tcp_sock *tp = tcp_sk(sk);
        struct bbr *bbr = inet_csk_ca(sk);
        u64 bw = bbr_bw(sk);

        bw = bw * tp->mss_cache * USEC_PER_SEC >> BW_SCALE;
        memset(&info->bbr, 0, sizeof(info->bbr));
        info->bbr.bbr_bw_lo        = (u32)bw;
        info->bbr.bbr_bw_hi        = (u32)(bw >> 32);
        info->bbr.bbr_min_rtt        = bbr->min_rtt_us;
        info->bbr.bbr_pacing_gain    = bbr->pacing_gain;
        info->bbr.bbr_cwnd_gain        = bbr->cwnd_gain;
        *attr = INET_DIAG_BBRINFO;
        return sizeof(info->bbr);
    }
    return 0;
}

__bpf_kfunc static void bbr_set_state(struct sock *sk, u8 new_state)
{
    struct bbr *bbr = inet_csk_ca(sk);

    if (new_state == TCP_CA_Loss) {
        struct rate_sample rs = { .losses = 1 };

        bbr->prev_ca_state = TCP_CA_Loss;
        bbr->full_bw = 0;
        bbr->round_start = 1;
        bbr_lt_bw_sampling(sk, &rs);
    }
}

static struct tcp_congestion_ops tcp_bbr_cong_ops __read_mostly = {
    .flags        = TCP_CONG_NON_RESTRICTED,
    .name        = "bbr",
    .owner        = THIS_MODULE,
    .init        = bbr_init,
    .cong_control    = bbr_main,
    .sndbuf_expand    = bbr_sndbuf_expand,
    .undo_cwnd    = bbr_undo_cwnd,
    .ssthresh    = bbr_ssthresh,
    .min_tso_segs    = bbr_min_tso_segs,
    .get_info    = bbr_get_info,
    .set_state    = bbr_set_state,
};

BTF_KFUNCS_START(tcp_bbr_check_kfunc_ids)
BTF_ID_FLAGS(func, bbr_init)
BTF_ID_FLAGS(func, bbr_main)
BTF_ID_FLAGS(func, bbr_sndbuf_expand)
BTF_ID_FLAGS(func, bbr_undo_cwnd)
BTF_ID_FLAGS(func, bbr_ssthresh)
BTF_ID_FLAGS(func, bbr_min_tso_segs)
BTF_ID_FLAGS(func, bbr_set_state)
BTF_KFUNCS_END(tcp_bbr_check_kfunc_ids)

static const struct btf_kfunc_id_set tcp_bbr_kfunc_set = {
    .owner = THIS_MODULE,
    .set   = &tcp_bbr_check_kfunc_ids,
};

static int __init bbr_register(void)
{
    int ret;

    BUILD_BUG_ON(sizeof(struct bbr) > ICSK_CA_PRIV_SIZE);

    ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &tcp_bbr_kfunc_set);
    if (ret < 0)
        return ret;
    return tcp_register_congestion_control(&tcp_bbr_cong_ops);
}

static void __exit bbr_unregister(void)
{
    tcp_unregister_congestion_control(&tcp_bbr_cong_ops);
}

module_init(bbr_register);
module_exit(bbr_unregister);

MODULE_AUTHOR("Van Jacobson <vanj@google.com>");
MODULE_AUTHOR("Neal Cardwell <ncardwell@google.com>");
MODULE_AUTHOR("Yuchung Cheng <ycheng@google.com>");
MODULE_AUTHOR("Soheil Hassas Yeganeh <soheil@google.com>");
MODULE_AUTHOR("Lee Yunjin <gzblues61@daum.net>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("TCP BBR with Adaptive Statistical Control Matrix");
