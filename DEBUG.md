# DEBUG.md — j293/t8103 AVD bring-up: complete investigation record

Status: **stopped, stable fallback shipped** (2026-10-13)
Machine: Apple MacBook Pro 13-inch M1 2020 (board j293, SoC t8103)
Kernel repo: `~/linux-m1`, branch `7.1.13-fairydust` (github.com/maintaining-m1-linux/linux)
Companion repo: `~/fox-builder` (github.com/maintaining-m1-linux/fox-builder, public) — Firefox AVD build scripts, firmware builder, and all macOS reverse-engineering docs
Current default boot: `7.1.13-fairydust-noavd-gdc896baa20aa` (stable, AVD off, battery fixed)
Backup kernel (NEVER delete): `7.0.13-fairydust-g80e49d7da3ab`

---

## 1. What this file is

This is the full context of the AVD (Apple Video Decoder) hardware-decode
bring-up attempt, written down so that neither a future session nor a future
human has to re-derive any of it. It records: what was tried, in what order,
what exactly happened (with the dmesg evidence), which hypotheses were killed
and by what experiment, and what remains unknown.

Read this together with the RE docs in `~/fox-builder/fox-builder-macos/`
(`AVD_POWER_RE.md`, `AVD_PMGR_RE.md`, `AVD_CLOCKGATE_LINUX.md`,
`AVD_LINUX_DISASM.md`, `AVD_MACOS_FINDINGS.md`, `AVD_BISECT_RESULT_1.md`,
`AVD_PREINIT_TESTING.md`) and the Linux-side logs (`AVD_LIVE_TEST_LOG.md`,
`research-avd-bringup.md` in `~/fox-builder/`).

---

## 2. Background and goal

The original porting work (late Sep 2026) put the Asahi fairydust kernel
(7.1.13) on this machine together with a custom-branded Firefox build
("Firefox (AVD)", non-official branding, installed to
`~/.local/opt/firefox-avd`, profile `~/.mozilla/firefox-avd`, launcher
command `installfox`, icon from `~/다운로드/icon.png` resized via ImageMagick).
That stack worked: Firefox 157.0 ran, web video played — but always with
**software decoding**.

The goal of the AVD effort was hardware video decode via the in-kernel
`apple-avd` driver.

Scope pivot that happened early and must not be forgotten:
**Gecko's Linux V4L2 path is stateful-m2m only, while `apple-avd` is a
stateless decoder.** Firefox HW decode therefore required either a
stateful→stateless translation layer inside Gecko or a driver that fakes
stateful operation — both large. The working plan became:

- Firefox keeps its custom AVD-branded build (SW decode, functional).
- Hardware decode target = **local playback** through Kwiboo's
  `v4l2-request` ffmpeg (pinned b57fbbe + frigate-asahi patches 0001/0002),
  which already contains the Annex-B → stateless translation layer
  (`fox-builder/build-ffmpeg-avd`).
- The remaining blocker for both: the `apple-avd` firmware handshake never
  completes on this machine. That is what the whole effort below was about.

A third-party claim of "Firefox + AVD HW decode working" (aquarat/frigate-asahi)
was tested by cloning that repo; it **failed** here (see §7).

---

## 3. Timeline of kernels and commits (all on `7.1.13-fairydust`)

Working-tree `.config` is gitignored; the *tracked* config source of truth is
`arch/arm64/configs/fairydust-extras.config` (merged on top of `asahi.config`).

Base era — known good:

| commit | date | content |
|---|---|---|
| `a68f016bc` | 09-27 | install_apple_dtb.sh: flat DTBs + update-m1n1 wiring |
| `e16a52a02` | 09-27 | drm/apple: don't advertise ARGB2101010 on overlay planes |
| `4c92a2e01` | 09-27 | drm/apple: snap page-flip timestamps to vblank grid |
| `3d3798693` | 09-27 | drm/asahi: report mismatch vs bootloader reference |
| `d811ba54c` | 09-27 | drm/asahi: debugfs power/performance status |
| `1c5ffe568` | 09-27 | arm64: configs: add fairydust-extras.config |
| `47947bedd` | 10-01 | media: apple: avd: fix H.264 segment allocation and slice leak |
| | | → built `7.1.13-fairydust-g47947beddce8` (#6), the reference stable kernel. GUI + Firefox worked on it from Oct 1. |

AVD experiment era (all reverted by `dc896baa2`, kept in history):

| commit | date | content |
|---|---|---|
| `6e6b8a4e9` | 10-01 | config: enable `CONFIG_VIDEO_APPLE_AVD=y` + embed `apple/avd-fw-v2-t0.bin` via `CONFIG_EXTRA_FIRMWARE` (fw built from AsahiLinux/avd-fw by fox-builder `setup-avd-fw`) |
| `833a2b6f1` | 10-01 | apply frigate-asahi decode stability patch series (kernel "avd2") |
| `60250cdd4` | 10-01 | fix avd->dev reference in refs_valid debug path |
| `08bd715e0` | 10-01 | build AVD as module again ("avd3") |
| `05e0fb8a3` | 10-01 | DEBUG: instrument PM error recording + genpd/pwrstate power_on (the "AVDBG" prints) |
| `957fa3050` | 10-02 | apply macOS enable/disableMCPUE sequence to CM3 boot ("avd4") |
| `78fcb6d28` | 10-02 | full t8103 bring-up sequence, macOS + m1n1 RE parity ("avd5") |
| `93b5488f9` | 10-02 | drop ADS reads (SError on unpowered block), stage markers ("avd6") |
| `8d837d0db` | 10-02 | log every preinit write to identify the SError source |
| `dd43d0587` | 10-04 | gate t8103 preinit stages behind `apple_avd.preinit_mask=` module param |
| `db5dac15c` | 10-05 | ads-probe stage (bit 6) |
| `8d310fec0` | 10-05 | macos-timing stage (bit 7) for the boot timeout |
| `4aa843f01` | 10-11 | ads-postrun (bit 8) + ps-kick (bit 9) for j293 ("avd11") |
| `b39358204` | 10-11 | AVDBGX on-screen progress markers, ps-kick mask fix ("avd12") |
| `f074a6efe` | 10-11 | **EMPTY commit** (message only): battery fix; `.config` is gitignored so nothing was committed. Working tree had `CONFIG_MACSMC_POWER=m`. |
| `dc896baa2` | 10-13 | **noavd**: revert all 14 AVD commits (`6e6b8a4e9^..b39358204`); fold `CONFIG_MACSMC_POWER=m` into the tracked fragment; LOCALVERSION `-fairydust-noavd`. Installed as default boot. |

Installed-kernel naming vs commits: LOCALVERSION carried `avd2..avd12` as the
user's experiment counter; the mapping to commits is in the table above.
`avd7` was deleted on user request, `avd10`/`avd11` during a /boot cleanup.

---

## 4. Environment and working conventions

- Debian trixie; gcc (Debian 14.2.0-19) 14.2.0; binutils 2.44.
- Boot chain: m1n1 → U-Boot (EFI v2.10) → GRUB → kernel.
  `root=UUID=fa135813-ec34-4d6e-bd85-c50684122f91 ro quiet log_buf_len=8M`.
  GRUB_TIMEOUT=3, menu shown.
- `install-fairydust-kernel.sh` (~/): side-by-side install; derives the
  release string from the built Image; runs modules_install, installs
  Image/System.map/config, `install_apple_dtb.sh` (DTBs + update-m1n1,
  updates `/boot/dtb` symlink), update-initramfs, grub-mkconfig.
  Refuses to overwrite an existing `/boot/vmlinuz-$REL`.
- Build: `make -j8` (~2 min) then `make -j8 dtbs` (install script requires
  `arch/arm64/boot/dts/apple/dtbs-list`).
- /boot is 1.5G — cleanup discipline required; never remove the 7.0.13 backup.
- No serial console. Failed boots leave **no log**; attribution relies on
  on-screen `pr_emerg` markers (that is why AVDBGX exists).
- Branch naming is exactly `7.1.13-fairydust` (user rejected
  `-improvements` suffixes). Docs and code comments in English.
- Keep everything in our own repos/branches; do not file upstream issues.
- User frequently works from a raw TTY (Korean does not render there);
  status replies in English.

---

## 5. The failure being debugged

On every AVD-enabled build, the driver probes and its firmware boot sequence
times out. Canonical dmesg (mask=0x00, representative of ~15 boots):

```
[    1.929996] avd 269080000.avd: AVDBG preinit: mask=0x00
[    1.940318] avd 269080000.avd: AVDBG boot: fw[0..2]=000007c0 00000001 0000082c rb=000007c0 00000001 0000082c rst=0
[    1.940324] avd 269080000.avd: AVDBG boot: fw 49152 bytes, hw 0000
[    1.940327] avd 269080000.avd: AVDBG boot: run_ctrl rb=00000001
[    1.950343] avd 269080000.avd: AVDBG boot: TIMEOUT flag0=00000000 mbox=de48ecd0
[    1.950350] avd 269080000.avd: AVDBG pm: runtime_error=-110 recorded (genpd_runtime_resume)
```

Facts contained in this:

- Firmware image (49152 bytes) is loaded into the code window and reads back
  correctly: `fw[0..2]` == image header `000007c0 00000001 0000082c`.
- The `hw` register reads `0x0000` (worth re-checking against macOS — never compared).
- `run_ctrl` start bit sticks: readback `rb=00000001`.
- The CM3 never finishes: ~10 ms later FLAG0 is still `0x00000000`, the wait
  times out with **-110 (ETIMEDOUT)**, and the PM core records the error at
  `genpd_runtime_resume`. All *subsequent* runtime resumes then return the
  recorded error (this is the full explanation of the sticky-error behavior;
  the earlier `-EINVAL` question was answered by this instrumentation).
- `mbox` dump values seen across boots: `dc48eed8`, `fc48ecd8`, `de48ecd0`,
  `fc48ecd0` — low bits cluster around `48ecd0-48eed8`; meaning unknown.

The power side is NOT the problem (from the same boots):

```
[    1.835097] apple-pmgr-pwrstate ...@358: AVDBG ps_set: mmx state 0xf ret=0 cur=0xf0003ff
[    1.835114] apple-pmgr-pwrstate ...@410: AVDBG ps_set: avd_sys state 0xf ret=0 cur=0xf0023ff
[    1.835119] genpd_provider avd_sys: AVDBG pwrstate: power_on ret=0
```

- `avd_sys` ps register is `0x23b700410`, powers on with **ret=0**, and its
  live value `0xf0023ff` **matches macOS exactly**.
- Parent `mmx` domain and the DART (`269010000.iommu`) resume cleanly, ret=0.
- genpd/pwrstate/SMC mailbox path fully functional.

---

## 6. Boot-failure taxonomy (with user corrections — read carefully)

Five distinct failure shapes occurred during the experiments. Do not conflate
them; user corrected several misattributions angrily and they are recorded here
as ground truth:

1. **Graceful AVD timeout** — boot succeeds, `avd_boot()` times out (-110),
   `failed to boot`, system otherwise fine. (Original fairydust behavior too.)
2. **Kernel panic** — `SError asynchronous ...`, panic trace mentions
   `avd runtime` / `avd boot`. Seen on avd5/avd6-era kernels.
3. **Freeze after m1n1 "handing off"** — screen shows the m1n1 handoff message,
   then nothing, forever, no panic text. Seen with most non-zero preinit
   stages. "t0 제외하고 모두 handoff 이후 죽음" — **all variants except the
   `preinit_mask=0x00` entry died after handing off.**
4. **GRUB blue-screen stuck at "Loading initial ramdisk..."** — the Debian
   GRUB blue screen shows that line and the machine hard-hangs (no SysRq,
   requires power button). USER CORRECTION: this is the **GRUB process itself
   freezing, before kernel handoff** — not a kernel crash, not Plymouth,
   and *removing `quiet` made it worse* (froze before even printing
   "Loading linux"). Cause unknown (suspect large kernel image/initrd read
   path, unproven). Do not theorize about kernel/AVD/PMGR for this mode.
5. **preinit 0x40-specific instant death** — writing/reading ADS registers
   pre-RUN hangs the machine immediately (confirmed by bisection:
   "0x40 adsprobe — hard hang on first ADS read").

Also: failed boots leave no logs anywhere the user knows of ("지난번 부팅
메시지 기록 남지 않아? 네가 볼 수는 없어?" — answer: no serial, no persistent
log from GRUB-era freezes; `journalctl -k -b -1` only helps when the boot
actually reached systemd).

---

## 7. Experiment results in detail

### 7.1 frigate-asahi (833a2b6f1)
Cloned aquarat/frigate-asahi after an explicit success report; applying its
decode-stability series did not change the boot timeout. Claim not reproduced.

### 7.2 PM instrumentation (05e0fb8a3)
Recorded `runtime_error` at every resume/suspend callback across PM core,
genpd, pwrstate, and serial/uart/mailbox drivers ("AVDBG pm: runtime_error=N
recorded (callback-name)"). Established:
- Only the AVD device ever records a nonzero error: -110 at
  `genpd_runtime_resume`, caused by the boot timeout, sticky afterwards.
- Every other device on the machine records 0 through suspend/resume cycles.
Killed the whole "PM core misbehaves" family of hypotheses.

### 7.3 ADS characterization (db5dac15c stage bit6, avd10 test)
With mask=0x80 (probe only after RUN): reading ADS status `0x269002010`
post-RUN is **safe** (no SError) and returns `0x1002010`; however the valid
bits `(val & 0x7f0)` stayed `0x00000000` through 500 ms of polling —
**ADS reports NOT-VALID forever**, and FLAG0 never set within 5 s.
So: ADS access post-RUN is not the hang; ADS simply never becomes valid.

### 7.4 Single-stage bisection matrix (fox-builder commit 9807f9a)
macOS driver RE was split into individual init stages exposed through
`apple_avd.preinit_mask` bits. Result: with per-stage probing, the **whole
AVD complex reads dark (0) except code/mbox/ctrl/dart** until deep into the
sequence — consistent with "firmware never runs", not with any single
missing register poke.

### 7.5 ps-kick and ads-postrun (4aa843f01, b39358204 — avd11/avd12)
Two new hypotheses from the macOS ARST ps decode: re-kick the ps register
after firmware load (bit 9), and write `0xfff` to ADS post-RUN (bit 8).
avd11's first boot froze after handing off (no log, unknown variant).
avd12 added AVDBGX on-screen markers (`T1` post-RUN, `PK1..PK6` around
ps-kick writes, `ADS1/2/3` around the 0xfff writes) and a GRUB matrix:
`v0=0x080` (sanity), `v1=0x200` (ps-kick only), `v2=0x100` (ads-postrun
only), `v3=0x380` (all). Matrix mechanism: `/etc/grub.d/42_avd8_test`
(`exec tail -n +3 $0` trick, entries differing only in `apple_avd.preinit_mask=`).

**Outcome: only v0 (mask=0x080, "t0") booted.** v1/v2 presumably froze
(consistent with §6.3) but the last AVDBGX line was never reported, so the
exact freeze point of ps-kick vs ads-postrun is UNKNOWN. If ps-kick freezes
at PK1/PK4 the whole genpd-foreign ps pokes must be abandoned; if ads-postrun
freezes at ADS1, ADS MMIO writes are dead in both pre- and post-RUN positions,
leaving "ADS power comes from the boot chain (m1n1/U-Boot) or PMC autonomy"
as the remaining direction.

### 7.6 Battery side discovery (f074a6efe → dc896baa2)
While testing AVD kernels it became obvious that **battery charge was missing
everywhere** (upower/GUI). Root cause found by diffing against the 7.0.13
backup config: upstream renamed `CONFIG_CHARGER_MACSMC` → `CONFIG_MACSMC_POWER`
during the 7.0→7.1 bump; the non-interactive oldconfig left the new symbol
unset. macsmc probed fine (rtc/gpio/reboot/input all present) but no
power_supply registered. Fixed by `CONFIG_MACSMC_POWER=m`, now committed in
the tracked fragment (f074a6efe was empty because `.config` is gitignored —
folded into dc896baa2).

### 7.7 GUI outage on all AVD kernels (UNRESOLVED)
From some point in the AVD era, Wayland/X11 stopped coming up on every
AVD kernel: sddm active with no children, no /var/log/sddm.log, Plasma
services (kded6/ksmserver/ksplash) all SIGABRT core-dumping at the same
second with `qt.qpa.wayland: Loading shell integration failed`. Kernel side
looked healthy (seat0, card0 MASTER, fb0 registered, /dev/dri ACLs fine,
dpkg 0 broken, Qt6 platform plugins present).
**Isolation experiment (boot the 7.0.13 backup kernel and check GUI) was
never run**, so kernel-vs-userspace ownership of this bug is unproven —
but the base fairydust kernel ran GUI fine on Oct 1 and the AVD commits touch
DART/pmgr/dtsi, so AVD patches are the prime suspect. The noavd revert is the
de-facto test: if GUI returns on noavd, it was the AVD patches.

---

## 8. Reverse-engineering record (macOS side, fox-builder-macos)

All docs in `~/fox-builder/fox-builder-macos/`, pushed. Key conclusions:

- **Physical mapping anchors (verified):** fw window kext `0x1080000` ==
  Linux `code` resource `0x269080000`; B15 self-reference `0x26907000` ==
  `0x269070000>>4`. mbox low offsets (+0x08/+0x48/+0x5c/+0x90…) match
  `avd-regs.h` exactly, so Linux resource bases are correct; only
  wrap/aux/ctrl2/ads needed hardcoded phys maps.
- **macOS init sequence (from driver disasm):** `initAvdWrap`:
  `0x269070024 ← 0x26907000`, C2 `0x269070000 ← 0`, C1 `0x269140018 ← 1`,
  DeviceInit C3–C8 at `0x269100000+` {`0x4064←3`,
  `0xcc90/0xcc94/0xccd0/0xccd4/0xcac8 ← 0xffffffff`}, then **ADS poll at
  `0x269002010` for `(val&0x7f0)==0x7f0`, 5 s timeout (diagnostic)**.
  Timeout path also one-shot-reads ADS status. Artifact:
  `~/avd-test/patched/apple-avd-macos-init.ko` (debugfs pmgr_read /
  force_power / do_boot hooks).
- **Clock-gate hypothesis: DEAD.** `AppleARMIO::enableDeviceClock` is a stub
  around `0xe00002c7`; gate `0x15d` has no consumers; no perf-floor property
  in DT (`AVD_CLOCKGATE_LINUX.md`).
- **SMC/perf-floor hypothesis: DEAD** (`AVD_POWER_RE.md`, `AVD_PMGR_RE.md`).
  ARST ps sequence fully decoded.
- **"VNOM/VMAX have dedicated ps registers": DEAD.** pmgr device-table
  decode: AVD_SYS id=0x66 flags=22 psidx=12 off=2 → register `0x23b700410`;
  AVD-SYS-V / VNOM / VMAX / AVD-SYS-DART are all VIRTUAL (psidx=0, off=0).
  psidx histogram across the table: {0:99, 1:30, 2:13, 3:10, 4:32, 5:26,
  6:2, 7:2, 8:5, 9:1, 10:1, 11:32, 12:15}.
- **Deferred candidates** (from bisect doc): pmgr perf-state floor / clock
  gates `0x15d`, `0x12a/0x12c/0x12d`; order sensitivity of DevicePwrOn vs
  firmware load.
- **Newest ioreg facts (not yet acted on):** the `avd` node carries
  `ads-present = <01000000>`; `dart-avd` has a third mapper
  `mapper-avd-adsbuf@2`. Whether t8103.dtsi covers the ADS DMA stream is
  UNCHECKED — this was the next lead when work stopped.
- macOS reverse work was delegated to an AI agent on the macOS side via
  `MACOS_REVERSE.md` instructions; the `rev/` workspace is tracked in
  fox-builder-macos. User explicitly refused "just update macOS" as a
  strategy — the work is pinned against the current installed macOS firmware
  version; that version should be recorded in the RE docs when known.
- Last data point destined for the macOS-side instructions: "everything except
  t0 died after m1n1 handing-off" (§6.3) — verify it landed in fox-builder.

---

## 9. What is proven vs unknown

### Proven (by direct experiment)
- genpd → pwrstate → SMC path powers `avd_sys` correctly; value matches macOS.
- Firmware image loads and reads back byte-correct.
- `run_ctrl` start bit sticks; CM3 still never signals FLAG0 → -110 timeout.
- ADS MMIO: pre-RUN access hangs the machine hard; post-RUN reads are safe
  but ADS stays NOT-VALID forever.
- The only PM error on the whole machine is the AVD -110; everything else 0.
- `preinit_mask=0x00` is the only AVD-kernel configuration that reaches
  userspace reliably; every non-zero stage tried (0x01, 0x40, 0x100, 0x200, …)
  either panicked, froze post-handoff, or froze GRUB.
- The battery bug was a config rename casualty, fixed in noavd.
- Third-party "it works" claims (frigate-asahi) did not reproduce here.

### Unknown
- **Why the CM3 does not run the loaded firmware** — clocks, reset line state,
  wrap-window setup, ADS power domain, or something in the boot chain
  (m1n1/U-Boot leave the complex in a different state than macOS's XNU path).
- Whether the GRUB-era freezes (§6.4) relate to AVD at all.
- GUI outage root cause (kernel AVD patches vs userspace; isolation test
  never run — noavd boot is the pending test).
- AVDBGX matrix v1/v2 freeze points (no logs; markers exist in history).
- t8103.dtsi vs ioreg `ads-present` / `mapper-avd-adsbuf@2` coverage.
- The `hw 0000` readback — never compared against macOS.

---

## 10. Current state (2026-10-13) and how to resume

Installed in /boot: `7.0.13-fairydust-g80e49d7da3ab` (backup) and
`7.1.13-fairydust-noavd-gdc896baa20aa` (default boot, includes the battery
fix). All avdN kernels and their module dirs were removed; `42_avd8_test`
deleted; /boot at 42%.

Branch state: HEAD `dc896baa2`; the 14 AVD commits remain in history directly
below. To resume AVD work:

```
git revert dc896baa2          # restores the exact avd12 tree state
# then rebuild LOCALVERSION to the next -fairydust-avdN
```

Suggested next leads, in order:
1. Boot noavd; confirm GUI is back and battery shows (closes §7.7).
2. Compare t8103.dtsi against ioreg: `ads-present`, `mapper-avd-adsbuf@2`,
   ADS stream coverage in dart-avd.
3. If resuming kernel experiments: wrap-stage retest (`mask=0x088`,
   bit7+bit3) — macOS calls the wrap/DMA window init "dark" every boot, so
   the earlier "dark" bisect conclusion may be contaminated; and `0x08e`
   (with dart stage). AVDBGX markers give freeze attribution without logs.
4. If ps-kick/ads-postrun resumed: read PK/ADS marker points first; abandon
   ps-register pokes if PK1/PK4 freezes, abandon ADS MMIO writes entirely if
   ADS1 freezes.
5. macOS-side: continue from `fox-builder-macos` deferred candidates
   (§8), with the "died after handing off" constraint added.

### Workflow cheatsheet
```
scripts/config --set-str LOCALVERSION "-fairydust-avdN"
make -j8 && make -j8 dtbs
printf '%s\n' '<pw>' | sudo -S bash ~/install-fairydust-kernel.sh
# GRUB default: edit GRUB_DEFAULT in /etc/default/grub to the full
# "Advanced options for Debian GNU/Linux>Debian GNU/Linux, with Linux <REL>"
# entry title, then update-grub
git push origin 7.1.13-fairydust
```
Rules: never delete the 7.0.13 backup; never force-push without asking;
docs and comments in English; branch name is exactly `7.1.13-fairydust`;
no speculation presented as fact — every claim needs a dmesg line or a
commit; failed boots leave no logs, so use on-screen pr_emerg markers.
