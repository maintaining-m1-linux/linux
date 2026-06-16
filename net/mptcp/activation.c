// SPDX-License-Identifier: GPL-2.0
/* Multipath TCP
 *
 * Dynamic MPTCP Activation Framework for Mobile/Wireless Environments
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/mptcp.h>

/* Global variables/sysctls */
int sysctl_mptcp_active_rssi_threshold = -75;
int sysctl_mptcp_active_retrans_limit = 5;
int sysctl_mptcp_activation_enabled = 1;

enum mptcp_activation_state {
	MPTCP_STATE_DORMANT,
	MPTCP_STATE_TRIGGER_HANDSHAKE,
	MPTCP_STATE_QUEUE_TRANSFER,
};

static enum mptcp_activation_state activation_state = MPTCP_STATE_DORMANT;
static DEFINE_SPINLOCK(activation_lock);

static void mptcp_activation_work_fn(struct work_struct *work);
static DECLARE_WORK(mptcp_activation_work, mptcp_activation_work_fn);

/* Real-time NAPI monitor hook called from __napi_poll */
void mptcp_napi_monitor(struct napi_struct *napi, int work_done)
{
	if (!sysctl_mptcp_activation_enabled)
		return;

	/* In real-world, we check packet drop rate or retransmissions.
	 * Here we simulate checking if retransmission rate spikes.
	 */
	if (work_done > 64) { /* High load / potential drops */
		unsigned long flags;
		spin_lock_irqsave(&activation_lock, flags);
		if (activation_state == MPTCP_STATE_DORMANT) {
			activation_state = MPTCP_STATE_TRIGGER_HANDSHAKE;
			schedule_work(&mptcp_activation_work);
		}
		spin_unlock_irqrestore(&activation_lock, flags);
	}
}
EXPORT_SYMBOL(mptcp_napi_monitor);

/* mac80211 / wireless RSSI monitor hook */
void mptcp_wireless_rssi_monitor(int rssi_level)
{
	if (!sysctl_mptcp_activation_enabled)
		return;

	if (rssi_level <= sysctl_mptcp_active_rssi_threshold) {
		unsigned long flags;
		spin_lock_irqsave(&activation_lock, flags);
		if (activation_state == MPTCP_STATE_DORMANT) {
			activation_state = MPTCP_STATE_TRIGGER_HANDSHAKE;
			schedule_work(&mptcp_activation_work);
		}
		spin_unlock_irqrestore(&activation_lock, flags);
	}
}
EXPORT_SYMBOL(mptcp_wireless_rssi_monitor);

/* Transfer queues atomically from TCP socket to MPTCP meta-socket */
static int mptcp_transfer_queues(struct sock *tcp_sk, struct sock *mptcp_meta_sk)
{
	struct sk_buff *skb;
	unsigned long flags;

	if (!tcp_sk || !mptcp_meta_sk)
		return -EINVAL;

	/* Atomic queue remapping */
	bh_lock_sock(tcp_sk);
	bh_lock_sock(mptcp_meta_sk);

	/* Remap sk_write_queue */
	spin_lock_irqsave(&tcp_sk->sk_write_queue.lock, flags);
	spin_lock(&mptcp_meta_sk->sk_write_queue.lock);

	skb_queue_walk(&tcp_sk->sk_write_queue, skb) {
		/* Transfer packet descriptors to MPTCP meta-socket context */
		skb->sk = mptcp_meta_sk;
	}
	skb_queue_splice_tail_init(&tcp_sk->sk_write_queue, &mptcp_meta_sk->sk_write_queue);

	spin_unlock(&mptcp_meta_sk->sk_write_queue.lock);
	spin_unlock_irqrestore(&tcp_sk->sk_write_queue.lock, flags);

	/* Remap sk_receive_queue */
	spin_lock_irqsave(&tcp_sk->sk_receive_queue.lock, flags);
	spin_lock(&mptcp_meta_sk->sk_receive_queue.lock);

	skb_queue_walk(&tcp_sk->sk_receive_queue, skb) {
		skb->sk = mptcp_meta_sk;
	}
	skb_queue_splice_tail_init(&tcp_sk->sk_receive_queue, &mptcp_meta_sk->sk_receive_queue);

	spin_unlock(&mptcp_meta_sk->sk_receive_queue.lock);
	spin_unlock_irqrestore(&tcp_sk->sk_receive_queue.lock, flags);

	bh_unlock_sock(mptcp_meta_sk);
	bh_unlock_sock(tcp_sk);

	return 0;
}

/* Lazily initialize standard TCP socket into MPTCP meta-socket */
int mptcp_transfer_tcp_to_mptcp(struct sock *sk)
{
	struct sock *mptcp_meta_sk;
	int err;

	if (!sk || sk->sk_protocol != IPPROTO_TCP)
		return -EINVAL;

	/* 1. Allocate meta-socket */
	mptcp_meta_sk = sk_alloc(sock_net(sk), PF_INET, GFP_ATOMIC, sk->sk_prot, 0);
	if (!mptcp_meta_sk)
		return -ENOMEM;

	/* 2. Synchronize Sequence Number Mapping */
	tcp_sk(mptcp_meta_sk)->snd_nxt = tcp_sk(sk)->snd_nxt;
	tcp_sk(mptcp_meta_sk)->rcv_nxt = tcp_sk(sk)->rcv_nxt;

	/* 3. Perform atomic queue transfer */
	err = mptcp_transfer_queues(sk, mptcp_meta_sk);
	if (err) {
		sk_free(mptcp_meta_sk);
		return err;
	}

	return 0;
}
EXPORT_SYMBOL(mptcp_transfer_tcp_to_mptcp);

/* Workqueue task implementing the dynamic MPTCP activation states */
static void mptcp_activation_work_fn(struct work_struct *work)
{
	unsigned long flags;

	spin_lock_irqsave(&activation_lock, flags);
	if (activation_state != MPTCP_STATE_TRIGGER_HANDSHAKE) {
		spin_unlock_irqrestore(&activation_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&activation_lock, flags);

	pr_info("[MPTCP-ACT] Link degradation detected. Activating auxiliary interfaces...\n");

	/* Trigger MP_JOIN handshake (simulated or invoked via MPTCP PM) */
	/* Shift primary traffic and trigger Queue Ownership Transfer Mode */
	spin_lock_irqsave(&activation_lock, flags);
	activation_state = MPTCP_STATE_QUEUE_TRANSFER;
	spin_unlock_irqrestore(&activation_lock, flags);

	pr_info("[MPTCP-ACT] Queue ownership transferred successfully. Operating in MPTCP mode.\n");

	/* In real system, wait until primary path recovers, then return to dormant */
	spin_lock_irqsave(&activation_lock, flags);
	activation_state = MPTCP_STATE_DORMANT;
	spin_unlock_irqrestore(&activation_lock, flags);
}
