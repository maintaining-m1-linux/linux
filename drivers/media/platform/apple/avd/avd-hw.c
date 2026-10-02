/* SPDX-License-Identifier: MIT */

#include "linux/dev_printk.h"
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/reset.h>

#include "avd.h"
#include "avd-regs.h"

/* The plan is to move this to the cm3 */

#define AVD_V3_VP_INSN_FIFO_IOVA	0x4068
#define AVD_V3_VP_INSN_FIFO_MASK	0x4084
#define AVD_V3_VP_INSN_FIFO_CACH	0x40a0
#define AVD_V3_VP_INSN_FIFO_XFER	0x40bc
#define AVD_V3_VP_CTRL_UNK	  0x4040
#define AVD_V3_CTRL_CM3_IRQ_MASK	0x405c

#define AVD_V4_VP_INSN_FIFO_IOVA_HI	0x30c
#define AVD_V4_VP_INSN_FIFO_IOVA_LO	0x150
#define AVD_V4_VP_INSN_FIFO_MASK	0x18c
#define AVD_V4_VP_INSN_FIFO_CACH	0x1c8
#define AVD_V4_VP_INSN_FIFO_XFER	0x204
#define AVD_V4_VP_CTRL_CM3_IRQ_MASK	0x0fc
#define AVD_V4_PP_CTRL_CM3_IRQ_MASK	0x120

/* seems stabile after this */
#define AVD_V5_VP_INSN_FIFO_IOVA_HI	0x20c
#define AVD_V5_VP_INSN_FIFO_IOVA_LO	0x1d0
#define AVD_V5_VP_INSN_FIFO_MASK	0x248
#define AVD_V5_VP_INSN_FIFO_CACH	0x284
#define AVD_V5_VP_INSN_FIFO_XFER	0x2c0
#define AVD_V5_VP_CTRL_CM3_IRQ_MASK	0x15c
#define AVD_V5_PP_CTRL_CM3_IRQ_MASK	0x190

#define AVD_CM3_UNK	  BIT(0)
#define AVD_CM3_ERROR	BIT(1)
#define AVD_CM3_DONE	BIT(2)
#define AVD_CM3_FULL	BIT(3)

#define AVD_VP_CM3_MASK (AVD_CM3_UNK | AVD_CM3_ERROR | AVD_CM3_DONE)
#define AVD_PP_CM3_MASK (AVD_CM3_UNK | AVD_CM3_DONE)

/*
 * Full t8103 preinit register tables, offsets relative to the 0x268000000
 * aperture.  Two independent sources agree on these sequences:
 *  - macOS 15.7.1 AppleAVD.kext v865 (arm64e kernelcache disasm; see
 *    fox-builder fox-builder-macos/AVD_LINUX_DISASM.md)
 *  - m1n1 proxyclient/m1n1/fw/avd/__init__.py (the original bring-up RE)
 */
struct avd_init_reg {
	u32 off;
	u32 val;
	u8 or; /* 1: value is ORed with the current register contents */
};

static const struct avd_init_reg t8103_wrap_init[] = {
	{ 0x1400014, 0x1, 0 },
	{ 0x1400018, 0x1, 0 },
	{ 0x1070000, 0x0, 0 },		/* PIODMA cfg */
	{ 0x1104064, 0x3, 0 },
	{ 0x110cc90, 0xffffffff, 0 },	/* IRQ clear */
	{ 0x110cc94, 0xffffffff, 0 },
	{ 0x110ccd0, 0xffffffff, 0 },
	{ 0x110ccd4, 0xffffffff, 0 },
	{ 0x110cac8, 0xffffffff, 0 },
	{ 0x1070024, 0x26907000, 0 },
	{ 0x1400014, 0x0, 0 },
};

static const struct avd_init_reg t8103_dma_tunables[] = {
	{ 0x1070024, 0x26907000, 0 },
	{ 0x1400000, 0x3, 0 },
	{ 0x1104000, 0x0, 0 },
	{ 0x110405c, 0x0, 0 },
	{ 0x1104110, 0x0, 0 },
	{ 0x11040f4, 0x1555, 0 },
	{ 0x1100000, 0xc0000000, 0 },
	{ 0x1101000, 0xc0000000, 0 },
	{ 0x1102000, 0xc0000000, 0 },
	{ 0x1103000, 0xc0000000, 0 },
	{ 0x1104000, 0xc0000000, 0 },
	{ 0x1105000, 0xc0000000, 0 },
	{ 0x1106000, 0xc0000000, 0 },
	{ 0x1107000, 0xc0000000, 0 },
	{ 0x1108000, 0xc0000000, 0 },
	{ 0x1109000, 0xc0000000, 0 },
	{ 0x110a000, 0xc0000000, 0 },
	{ 0x110b000, 0xc0000000, 0 },
	{ 0x110c010, 0x1, 0 },
	{ 0x110c018, 0x1, 0 },
	{ 0x110c040, 0xc0000000, 1 },
	{ 0x110c080, 0xc0000000, 1 },
	{ 0x110c0c0, 0xc0000000, 1 },
	{ 0x110c100, 0xc0000000, 1 },
	{ 0x110c140, 0xc0000000, 1 },
	{ 0x110c180, 0xc0000000, 1 },
	{ 0x110c1c0, 0xc0000000, 1 },
	{ 0x110c200, 0xc0000000, 1 },
	{ 0x110c240, 0xc0000000, 1 },
	{ 0x110c280, 0xc0000000, 1 },
	{ 0x110c2c0, 0xc0000000, 1 },
	{ 0x110c300, 0xc0000000, 1 },
	{ 0x110c340, 0xc0000000, 1 },
	{ 0x110c380, 0xc0000000, 1 },
	{ 0x110c3c0, 0xc0000000, 1 },
	{ 0x110c400, 0xc0000000, 1 },
	{ 0x110c440, 0xc0000000, 1 },
	{ 0x110c480, 0xc0000000, 1 },
	{ 0x110c4c0, 0xc0000000, 1 },
	{ 0x110c500, 0xc0000000, 1 },
	{ 0x110c540, 0xc0000000, 1 },
	{ 0x110c580, 0xc0000000, 1 },
	{ 0x110c5c0, 0xc0000000, 1 },
	{ 0x110c600, 0xc0000000, 1 },
	{ 0x110c640, 0xc0000000, 1 },
	{ 0x110c680, 0xc0000000, 1 },
	{ 0x110c6c0, 0xc0000000, 1 },
	{ 0x110c700, 0xc0000000, 1 },
	{ 0x110c740, 0xc0000000, 1 },
	{ 0x110c780, 0xc0000000, 1 },
	{ 0x110c7c0, 0xc0000000, 1 },
	{ 0x110c800, 0xc0000000, 1 },
	{ 0x110c840, 0xc0000000, 1 },
	{ 0x110c880, 0xc0000000, 1 },
	{ 0x110c8c0, 0xc0000000, 1 },
	{ 0x110c900, 0xc0000000, 1 },
	{ 0x110c940, 0xc0000000, 1 },
	{ 0x110c980, 0xc0000000, 1 },
	{ 0x110c9c0, 0xc0000000, 1 },
	{ 0x110ca00, 0xc0000000, 1 },
	{ 0x110ca40, 0xc0000000, 1 },
	{ 0x110ca80, 0xc0000000, 1 },
	{ 0x110cac0, 0xc0000000, 1 },
	{ 0x110cb00, 0xc0000000, 1 },
	{ 0x110cb40, 0xc0000000, 1 },
	{ 0x110cb80, 0xc0000000, 1 },
	{ 0x110cbc0, 0xc0000000, 1 },
	{ 0x110cc00, 0xc0000000, 1 },
	{ 0x110cc40, 0xc0000000, 1 },
	{ 0x110cc80, 0xc0000000, 1 },
	{ 0x110ccc0, 0xc0000000, 1 },
	{ 0x110cd00, 0xc0000003, 1 },
	{ 0x110c044, 0x40, 0 },
	{ 0x110c084, 0x400040, 0 },
	{ 0x110c244, 0x800034, 0 },
	{ 0x110c284, 0x18, 0 },
	{ 0x110c2c4, 0xb40020, 0 },
	{ 0x110c3c4, 0xd40030, 0 },
	{ 0x110c404, 0x180014, 0 },
	{ 0x110c444, 0x104001c, 0 },
	{ 0x110c484, 0x2c0014, 0 },
	{ 0x110c4c4, 0x1200014, 0 },
	{ 0x110c504, 0x400018, 0 },
	{ 0x110c544, 0x1340024, 0 },
	{ 0x110c584, 0x580014, 0 },
	{ 0x110c5c4, 0x1580014, 0 },
	{ 0x110c1c4, 0x6c0048, 0 },
	{ 0x110c204, 0xb40048, 0 },
	{ 0x110c384, 0xfc0038, 0 },
	{ 0x110c604, 0x1340030, 0 },
	{ 0x110c644, 0x16c00b0, 0 },
	{ 0x110c684, 0x21c00b0, 0 },
	{ 0x110c844, 0x164001c, 0 },
	{ 0x110c884, 0x2cc0028, 0 },
	{ 0x110c744, 0x1800018, 0 },
	{ 0x110c784, 0x2f40020, 0 },
	{ 0x110c7c4, 0x1980018, 0 },
	{ 0x110c804, 0x314001c, 0 },
	{ 0x110c8c4, 0x1b00024, 0 },
	{ 0x110c904, 0x3300040, 0 },
	{ 0x110c944, 0x1d4001c, 0 },
	{ 0x110c984, 0x370002c, 0 },
	{ 0x110c9c4, 0x1f00030, 0 },
	{ 0x110ca04, 0x39c003c, 0 },
	{ 0x110ca44, 0x2200014, 0 },
	{ 0x110ca84, 0x3d80014, 0 },
	{ 0x110cb04, 0x2340014, 0 },
	{ 0x110cb44, 0x3ec0014, 0 },
	{ 0x110cac4, 0x2480080, 0 },
	{ 0x110cc8c, 0x2c80014, 0 },
	{ 0x110cccc, 0x2dc0014, 0 },
	{ 0x110cc88, 0x2f00060, 0 },
	{ 0x110ccc8, 0x3500054, 0 },
	{ 0x110cb84, 0x3a4001c, 0 },
	{ 0x110cbc4, 0x4000040, 0 },
	{ 0x110cc04, 0x3c00040, 0 },
	{ 0x110cc44, 0x44000c0, 0 },
	{ 0x110405c, 0x500000, 1 },
	{ 0x109807c, 0x1, 0 },
	{ 0x1098080, 0xffffffff, 0 },
};

static void avd_t8103_preinit(struct avd_dev *avd)
{
	void __iomem *f = avd->full;
	void __iomem *pmgr;
	u32 v;
	int i, ret;

	/* macOS CAvdWrapCtrlViola::DevicePwrOn: power the ADS block */
	writel(0xfff, f + AVD_OFF_ADS_PWR);

	/* macOS AppleAVD::waitValidADSStatus: (status & 0x7f0) == 0x7f0 */
	ret = readl_poll_timeout(f + AVD_OFF_ADS_STATUS, v,
				 (v & 0x7f0) == 0x7f0, 10000, 5000000);
	dev_info(avd->dev, "AVDBG boot: ads=%08x (%s)\n",
		 readl(f + AVD_OFF_ADS_STATUS), ret ? "NOT READY" : "ready");

	/* dart-avd init masks (m1n1 + macOS agree) */
	writel(readl(f + AVD_OFF_DART_0) | AVD_DART_MASK_0, f + AVD_OFF_DART_0);
	writel(readl(f + AVD_OFF_DART_1) | AVD_DART_MASK_1, f + AVD_OFF_DART_1);
	writel(readl(f + AVD_OFF_DART_2) | AVD_DART_MASK_2, f + AVD_OFF_DART_2);

	/* macOS CAvdMcpu::init(clearDMEM): clear SRAM */
	memset_io(f + 0x108c000, 0, 0xc000);

	for (i = 0; i < ARRAY_SIZE(t8103_wrap_init); i++)
		writel(t8103_wrap_init[i].val, f + t8103_wrap_init[i].off);

	for (i = 0; i < ARRAY_SIZE(t8103_dma_tunables); i++) {
		v = t8103_dma_tunables[i].val;
		if (t8103_dma_tunables[i].or)
			v |= readl(f + t8103_dma_tunables[i].off);
		writel(v, f + t8103_dma_tunables[i].off);
	}

	/* read-only diagnostic: pmgr ps registers around avd_sys (@0x410) */
	pmgr = ioremap(AVD_PMGR_BASE_PHYS + 0x3a0, 0x100);
	if (pmgr) {
		dev_info(avd->dev, "AVDBG boot: pmgr[0x3a0..0x490] %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 readl(pmgr + 0x000), readl(pmgr + 0x008),
			 readl(pmgr + 0x010), readl(pmgr + 0x018),
			 readl(pmgr + 0x020), readl(pmgr + 0x028),
			 readl(pmgr + 0x030), readl(pmgr + 0x038),
			 readl(pmgr + 0x040), readl(pmgr + 0x048),
			 readl(pmgr + 0x050), readl(pmgr + 0x058),
			 readl(pmgr + 0x060), readl(pmgr + 0x068),
			 readl(pmgr + 0x070), readl(pmgr + 0x078),
			 readl(pmgr + 0x080), readl(pmgr + 0x088),
			 readl(pmgr + 0x090), readl(pmgr + 0x098),
			 readl(pmgr + 0x0a0), readl(pmgr + 0x0a8),
			 readl(pmgr + 0x0b0), readl(pmgr + 0x0b8),
			 readl(pmgr + 0x0c0), readl(pmgr + 0x0c8),
			 readl(pmgr + 0x0d0), readl(pmgr + 0x0d8),
			 readl(pmgr + 0x0e0), readl(pmgr + 0x0e8),
			 readl(pmgr + 0x0f0), readl(pmgr + 0x0f8));
		iounmap(pmgr);
	}
}

int avd_boot(struct avd_dev *avd)
{
	u32 val;
	int ret;

	if (avd->variant->revision != 3)
		dev_info_once(avd->dev, "booting hw version: %04x",
				readl_relaxed(avd->ctrl));

	/* Full t8103 bring-up: ADS power, DART, wrap ctrl, DMA tunables.
	 * No-op on hardware where the boot chain already did this (j274). */
	if (avd->variant->revision == 3 && avd->full)
		avd_t8103_preinit(avd);

	/* Quiesce the coprocessor first, mirroring macOS AppleAVD.kext
	 * CAvdM3Mcpu::disableMCPUE() (disasm, macOS 15.7.1 v865, t8103). */
	writel_relaxed(AVD_RUN_CTRL_UNK_STOP, avd->mbox + AVD_REG_RUN_CTRL);
	writel_relaxed(1, avd->mbox + AVD_REG_FLAG0_CLR);
	writel_relaxed(0, avd->mbox + AVD_REG_MCPUE_UNK10);
	writel_relaxed(0, avd->mbox + AVD_REG_MCPUE_UNK50);

	memcpy_toio(avd->code, avd->fw->data, avd->fw->size);

	dev_info(avd->dev,
		 "AVDBG boot: fw[0..2]=%08x %08x %08x rb=%08x %08x %08x rst=%d\n",
		 ((u32 *)avd->fw->data)[0], ((u32 *)avd->fw->data)[1],
		 ((u32 *)avd->fw->data)[2],
		 readl_relaxed(avd->code), readl_relaxed(avd->code + 4),
		 readl_relaxed(avd->code + 8),
		 avd->rstc ? reset_control_status(avd->rstc) : -999);

	dev_info(avd->dev, "AVDBG boot: fw %zu bytes, hw %04x\n",
		 avd->fw->size, readl_relaxed(avd->ctrl));

	/* Power up the coprocessor, mirroring CAvdM3Mcpu::enableMCPUE()
	 * and the m1n1 fw/avd mcpu start sequence (they agree):
	 * +0x50/+0x68/+0x74 first, mailbox enables, then +0x10=2 and
	 * +0x48=8 ("enable mailbox interrupts" in m1n1), RUN last. */
	writel_relaxed(1, avd->mbox + AVD_REG_MCPUE_UNK50);
	writel_relaxed(1, avd->mbox + AVD_REG_MCPUE_UNK68);
	writel_relaxed(AVD_MBOX_ENABLE, avd->mbox + AVD_REG_MBOX1_STATUS);
	writel_relaxed(1, avd->mbox + AVD_REG_MCPUE_UNK74);
	writel_relaxed(2, avd->mbox + AVD_REG_MCPUE_UNK10);
	writel_relaxed(AVD_MBOX1_NOT_EMPTY, avd->mbox + AVD_REG_MBOX_IRQ_ENABLE);
	writel_relaxed(AVD_RUN_CTRL_UNK_RUN, avd->mbox + AVD_REG_RUN_CTRL);
	dev_info(avd->dev, "AVDBG boot: run_ctrl rb=%08x\n",
		 readl_relaxed(avd->mbox + AVD_REG_RUN_CTRL));

	/* wait for cm3 to boot */
	ret = readl_poll_timeout(avd->mbox + AVD_REG_FLAG0_SET,
			val, val == 1, 10, 10000);
	if (ret) {
		dev_info(avd->dev, "AVDBG boot: TIMEOUT flag0=%08x mbox=%08x ads=%08x\n",
			 readl_relaxed(avd->mbox + AVD_REG_FLAG0_SET),
			 readl_relaxed(avd->mbox + AVD_REG_MBOX1_RETRIEVE),
			 avd->full ? readl(avd->full + AVD_OFF_ADS_STATUS) : 0);
		return ret;
	}

	dev_info(avd->dev, "AVDBG boot: OK\n");
	return 0;
}

void avd_shutdown(struct avd_dev *avd)
{
	writel_relaxed(AVD_RUN_CTRL_UNK_STOP, avd->mbox + AVD_REG_RUN_CTRL);
	writel_relaxed(1, avd->mbox + AVD_REG_FLAG0_CLR);
	writel_relaxed(0, avd->mbox + AVD_REG_MBOX_IRQ_ENABLE);
}

#define w32(reg, val) (writel_relaxed(val, avd->ctrl + (reg)))
#define m32(reg, val) (w32(reg, (val) | readl_relaxed(avd->ctrl + (reg))))

void t8103_configure_stream(struct avd_dev *avd, dma_addr_t addr, u8 fifo_idx,
			  u32 vp_slot)
{
	w32(AVD_V3_VP_INSN_FIFO_IOVA + (fifo_idx * 4), addr >> 8);
	w32(AVD_V3_VP_INSN_FIFO_MASK + (fifo_idx * 4), 0);
	w32(AVD_V3_VP_INSN_FIFO_CACH + (fifo_idx * 4), 0);
	w32(AVD_V3_VP_INSN_FIFO_XFER + (fifo_idx * 4), 0);

	w32(AVD_V3_VP_CTRL_UNK + (vp_slot * 4), 0);
	m32(AVD_V3_CTRL_CM3_IRQ_MASK,
	    AVD_VP_CM3_MASK << (vp_slot * 5) | (AVD_PP_CM3_MASK << 20));
}

void t8112_configure_stream(struct avd_dev *avd, dma_addr_t addr, u8 fifo_idx,
			  u32 vp_slot)
{
	if (avd->variant->quirks & AVD_QUIRK_LSR) {
		w32(AVD_V4_VP_INSN_FIFO_IOVA_LO + (fifo_idx * 4), addr >> 8);
	} else {
		w32(AVD_V4_VP_INSN_FIFO_IOVA_HI + (fifo_idx * 4), addr >> 32);
		w32(AVD_V4_VP_INSN_FIFO_IOVA_LO + (fifo_idx * 4), addr & 0xffffffff);
	}
	w32(AVD_V4_VP_INSN_FIFO_MASK + (fifo_idx * 4), 0);
	w32(AVD_V4_VP_INSN_FIFO_CACH + (fifo_idx * 4), 0);
	w32(AVD_V4_VP_INSN_FIFO_XFER + (fifo_idx * 4), 0);

	m32(AVD_V4_VP_CTRL_CM3_IRQ_MASK + (vp_slot * 4), AVD_VP_CM3_MASK);
	m32(AVD_V4_PP_CTRL_CM3_IRQ_MASK, AVD_PP_CM3_MASK);
}

void t8122_configure_stream(struct avd_dev *avd, dma_addr_t addr, u8 fifo_idx,
			  u32 vp_slot)
{
	w32(AVD_V5_VP_INSN_FIFO_IOVA_HI + (fifo_idx * 4), addr >> 32);
	w32(AVD_V5_VP_INSN_FIFO_IOVA_LO + (fifo_idx * 4), addr & 0xffffffff);
	w32(AVD_V5_VP_INSN_FIFO_MASK + (fifo_idx * 4), 0);
	w32(AVD_V5_VP_INSN_FIFO_CACH + (fifo_idx * 4), 0);
	w32(AVD_V5_VP_INSN_FIFO_XFER + (fifo_idx * 4), 0);

	m32(AVD_V5_VP_CTRL_CM3_IRQ_MASK + (vp_slot * 4), AVD_VP_CM3_MASK);
	m32(AVD_V5_PP_CTRL_CM3_IRQ_MASK, AVD_PP_CM3_MASK);
}

void avd_status(struct avd_dev *avd, u32 vp)
{
	/*
	 * the command 0x2d000000 resets theese
	 *
	 * 0  config 0x80000000 or 0xc0000000
	 * 1  related to buffers? example 0x0e6733a0
	 * 2  unkown zero
	 * 3  unkown zero
	 * 4  mb parsed? starts at zero same as h264 cm3_set_mb_dims
	 * 5  status 0x3f on succes, could be usefull
	 * 6  slice bytes parsed
	 * 7  insn bytes written/parsed
	 */
	u32 start;
	u32 val[8];
	if (avd->variant->revision == 3)
		start = vp << 12;
	else
		start = 0x1000 | (vp << 8);

	for (int i = 0; i < 8; i++)
		val[i] = readl_relaxed(avd->ctrl + start + (i * 4));

	dev_info(avd->dev, "VP%d: %08x %08x %08x %08x", vp, val[0], val[1],
		 val[2], val[3]);
	dev_info(avd->dev, "VP%d: %08x %08x %08x %08x", vp, val[4], val[5],
		 val[6], val[7]);
}

#undef w32
#undef r32
#undef m32
