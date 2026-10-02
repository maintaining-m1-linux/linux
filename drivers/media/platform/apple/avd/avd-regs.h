/* SPDX-License-Identifier: MIT */

#ifndef AVD_REGS_H_
#define AVD_REGS_H_

#define AVD_REG_RUN_CTRL	0x08
#define AVD_RUN_CTRL_UNK_RUN	BIT(0)
#define AVD_RUN_CTRL_UNK_STOP	BIT(1) | BIT(2) | BIT(3)

#define AVD_REG_MBOX_IRQ_ENABLE	0x48
#define AVD_REG_MBOX_IRQ_CLR	0x4c
#define AVD_MBOX1_EMPTY	BIT(2)
#define AVD_MBOX1_NOT_EMPTY	BIT(3)

#define AVD_REG_MBOX1_STATUS	0x5c
#define AVD_REG_MBOX1_RETRIEVE	0x64
#define AVD_MBOX_ENABLE	BIT(0)

#define AVD_REG_FLAG0_SET	0x90
#define AVD_REG_FLAG0_CLR	0x98

/*
 * Coprocessor (CM3) power/clock control registers in the mailbox page,
 * written by macOS CAvdM3Mcpu::enableMCPUE()/disableMCPUE() on every boot.
 * +0x50 is cleared then set around the firmware load; +0x68/+0x74 are set
 * before RUN_CTRL.  Without these the CM3 never starts on some machines.
 */
#define AVD_REG_MCPUE_UNK10	0x10
#define AVD_REG_MCPUE_UNK50	0x50
#define AVD_REG_MCPUE_UNK68	0x68
#define AVD_REG_MCPUE_UNK74	0x74

/*
 * The Apple DT maps the whole AVD complex as one window:
 * base 0x268000000, size 0x1404000 (verified against macOS AppleAVD.kext
 * v865 register tables and m1n1 proxyclient/m1n1/fw/avd/__init__.py).
 * Offsets below are relative to that base.
 */
#define AVD_FULL_BASE_PHYS	0x268000000ULL
#define AVD_FULL_SIZE		0x1404000

/* ADS (0x269000000 block): power/valid registers */
#define AVD_OFF_ADS_PWR		0x1000000	/* macOS DevicePwrOn: = 0xfff */
#define AVD_OFF_ADS_STATUS	0x1002010	/* macOS waits (val & 0x7f0) == 0x7f0 */

/* DART (dart-avd at 0x269010000): init masks (m1n1 + macOS) */
#define AVD_OFF_DART_0		0x1010060
#define AVD_DART_MASK_0		0x80016100
#define AVD_OFF_DART_1		0x1010068
#define AVD_DART_MASK_1		0xf0f0f
#define AVD_OFF_DART_2		0x101006c
#define AVD_DART_MASK_2		0x80808

/* macOS t8103 PMGR ps register of avd_sys (from t8103-pmgr.dtsi @0x410) */
#define AVD_PMGR_BASE_PHYS	0x23b700000ULL
#define AVD_PMGR_AVD_SYS	0x410

#endif /* AVD_REGS_H_ */
