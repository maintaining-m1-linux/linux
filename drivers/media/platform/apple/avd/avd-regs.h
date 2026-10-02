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

#endif /* AVD_REGS_H_ */
