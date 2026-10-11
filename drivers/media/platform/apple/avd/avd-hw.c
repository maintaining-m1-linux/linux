/* SPDX-License-Identifier: MIT */

#include "linux/dev_printk.h"
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/module.h>
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
 *
 * Every stage is individually selectable via the preinit_mask module
 * parameter so a bad stage can be isolated without rebuilding:
 *
 *   bit 0  ADS block power write (0x269000000 = 0xfff)
 *   bit 1  DART-AVD init masks
 *   bit 2  SRAM clear
 *   bit 3  wrap ctrl init table
 *   bit 4  DMA tunables table
 *   bit 5  pmgr ps dump (read-only diagnostic)
 *   bit 6  ads-probe: characterize ADS register access on j293 (reads
 *            first, then the DevicePwrOn write with readback, then the
 *            macOS waitValidADSStatus poll).  Runs before stage 1.
 *   bit 7  macos-timing: sleep 726 ms after power-on and before fw
 *            upload (macOS setPowerStateOn window), then poll the ADS
 *            valid bits (ctrl+0x1002010 & 0x7f0, up to 500x10 ms) after
 *            RUN_CTRL, and extend the FLAG0 wait to 5 s.  Diagnostic
 *            only — never fails the boot.  See
 *            fox-builder fox-builder-macos/AVD_CLOCKGATE_LINUX.md §6.1:
 *            macOS does no clock-gate MMIO/SMC message for AVD, so the
 *            remaining deltas are timing and patience.
 *            avd10/t0 (mask=0x80): safe (reads OK post-RUN), but ADS
 *            never becomes valid and FLAG0 never rises.
 *   bit 8  ads-postrun: on FLAG0 timeout, retry the ADS power write
 *            (0x269000000 <- 0xfff — the j274/m1n1 DevicePwrOn write that
 *            SError-panics pre-RUN on j293) AFTER RUN_CTRL, then re-poll
 *            ADS-valid and FLAG0 (5 s each).  Tests whether the write is
 *            accepted post-RUN and whether an unpowered ADS block is what
 *            blocks the CM3 firmware from setting FLAG0.
 *   bit 9  ps-kick: re-cycle AVD_SYS ps off/on (target 0 -> 0xf with the
 *            exact apple_pmgr_ps_set choreography + AUTO_ENABLE) at
 *            preinit time.  Linux powers AVD_SYS at ~1.9 s, possibly
 *            before the PMC applies rails; macOS powers the block seconds
 *            after boot through its power-state chain.  Re-kicks the
 *            power when the system is settled.  Runs before bit 7's
 *            sleep so the block gets the same 726 ms settle.
 *
 * Default 0 runs no preinit (avd4-equivalent: MCPUE boot sequence only)
 * and is always safe to boot.  Pass apple_avd.preinit_mask=<mask> on the
 * kernel command line; the driver is built in so the value can be changed
 * per boot from the GRUB editor.  dmesg prints the mask at boot and the
 * last "AVDBG wr:" line before a crash identifies the faulting register.
 */
#define AVD_PREINIT_ADS		BIT(0)
#define AVD_PREINIT_DART	BIT(1)
#define AVD_PREINIT_SRAM	BIT(2)
#define AVD_PREINIT_WRAP	BIT(3)
#define AVD_PREINIT_TUNABLES	BIT(4)
#define AVD_PREINIT_PMGR	BIT(5)
#define AVD_PREINIT_PROBE	BIT(6)
#define AVD_PREINIT_TIMING	BIT(7)
#define AVD_PREINIT_ADS_POSTRUN	BIT(8)
#define AVD_PREINIT_PSKICK	BIT(9)

static int preinit_mask;
module_param(preinit_mask, int, 0644);
MODULE_PARM_DESC(preinit_mask,
	"t8103 preinit stage mask: 0=none (default, safe), bit0 ads bit1 dart "
	"bit2 sram bit3 wrap bit4 tunables bit5 pmgr-dump bit6 ads-probe "
	"bit7 macos-timing (726 ms settle + ADS-valid poll + 5 s FLAG0 wait) "
	"bit8 ads-postrun (retry: write ADS pwr 0xfff after RUN + re-poll) "
	"bit9 ps-kick (re-cycle AVD_SYS ps off/on through the genpd sequence)");

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

/* Log every preinit write: the last printed line before a crash
 * identifies the faulting register. */
static void avd_preinit_write(struct avd_dev *avd, u32 off, u32 val, int or)
{
	if (or)
		dev_info(avd->dev, "AVDBG wr: [%08x] |= %08x (was %08x)\n",
			 off, val, readl(avd->full + off));
	else
		dev_info(avd->dev, "AVDBG wr: [%08x] = %08x\n", off, val);
	if (or)
		val |= readl(avd->full + off);
	writel(val, avd->full + off);
}

static void avd_t8103_preinit(struct avd_dev *avd)
{
	void __iomem *f = avd->full;
	void __iomem *pmgr;
	int i;

	dev_info(avd->dev, "AVDBG preinit: mask=0x%02x\n", preinit_mask);

	/* ADS access characterization (j293): reads first — nobody has ever
	 * read the ADS block on this machine; avd6 dropped reads preemptively
	 * and stage 1 always ran first in avd5/6/7.  If the reads survive,
	 * perform the macOS DevicePwrOn write with readback and then poll the
	 * status the way CAvdApCommViola::waitValidADSStatus does (mask 0x7f0). */
	if (preinit_mask & AVD_PREINIT_PROBE) {
		u32 st, pwr;
		int i;

		dev_info(avd->dev, "AVDBG preinit: stage 0/6 ads-probe (reads first)\n");
		st = readl(avd->full + AVD_OFF_ADS_STATUS);
		dev_info(avd->dev, "AVDBG adsprobe: status(0x1002010)=%08x\n", st);
		pwr = readl(avd->full + AVD_OFF_ADS_PWR);
		dev_info(avd->dev, "AVDBG adsprobe: pwr(0x1000000)=%08x\n", pwr);
		dev_info(avd->dev, "AVDBG adsprobe: writing 0xfff\n");
		writel(0xfff, avd->full + AVD_OFF_ADS_PWR);
		dev_info(avd->dev, "AVDBG adsprobe: readback=%08x\n",
			 readl(avd->full + AVD_OFF_ADS_PWR));
		for (i = 0; i < 20; i++) {
			st = readl(avd->full + AVD_OFF_ADS_STATUS);
			dev_info(avd->dev, "AVDBG adsprobe: poll[%d] status=%08x\n", i, st);
			if ((st & 0x7f0) == 0x7f0)
				break;
			msleep(10);
		}
	}

	/* macOS CAvdWrapCtrlViola::DevicePwrOn: power the ADS block.
	 * Write-only here: reading the ADS block before its power domain
	 * is up faults (SError) — macOS polls it only after full power-on. */
	if (preinit_mask & AVD_PREINIT_ADS) {
		dev_info(avd->dev, "AVDBG preinit: stage 1/6 ads-pwr (w 0x1000000=0xfff)\n");
		avd_preinit_write(avd, AVD_OFF_ADS_PWR, 0xfff, 0);
	}

	/* dart-avd init masks (m1n1 + macOS agree) */
	if (preinit_mask & AVD_PREINIT_DART) {
		dev_info(avd->dev, "AVDBG preinit: stage 2/6 dart masks\n");
		avd_preinit_write(avd, AVD_OFF_DART_0, AVD_DART_MASK_0, 1);
		avd_preinit_write(avd, AVD_OFF_DART_1, AVD_DART_MASK_1, 1);
		avd_preinit_write(avd, AVD_OFF_DART_2, AVD_DART_MASK_2, 1);
	}

	/* macOS CAvdMcpu::init(clearDMEM) + m1n1 fw/avd boot(): clear CODE
	 * (0x1080000) and SRAM (0x108c000) */
	if (preinit_mask & AVD_PREINIT_SRAM) {
		dev_info(avd->dev, "AVDBG preinit: stage 3/6 code+sram clear\n");
		memset_io(f + 0x1080000, 0, 0xc000);
		memset_io(f + 0x108c000, 0, 0xc000);
	}

	if (preinit_mask & AVD_PREINIT_WRAP) {
		dev_info(avd->dev, "AVDBG preinit: stage 4/6 wrap init (%zu regs)\n",
			 ARRAY_SIZE(t8103_wrap_init));
		for (i = 0; i < ARRAY_SIZE(t8103_wrap_init); i++)
			avd_preinit_write(avd, t8103_wrap_init[i].off,
					  t8103_wrap_init[i].val, t8103_wrap_init[i].or);
	}

	if (preinit_mask & AVD_PREINIT_TUNABLES) {
		dev_info(avd->dev, "AVDBG preinit: stage 5/6 dma tunables (%zu regs)\n",
			 ARRAY_SIZE(t8103_dma_tunables));
		for (i = 0; i < ARRAY_SIZE(t8103_dma_tunables); i++)
			avd_preinit_write(avd, t8103_dma_tunables[i].off,
					  t8103_dma_tunables[i].val, t8103_dma_tunables[i].or);
	}

	/* read-only diagnostic: pmgr ps registers around avd_sys (@0x410) */
	if (preinit_mask & AVD_PREINIT_PMGR) {
		dev_info(avd->dev, "AVDBG preinit: stage 6/6 pmgr dump\n");
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

	/* avd10/t0 result (mask=0x80): ADS status reads are safe after RUN_CTRL
	 * (no SError), but the valid bits (0x1002010 & 0x7f0) stay 0 forever and
	 * FLAG0 never rises even with 5 s patience.  macOS j293 powers the ADS
	 * block without the 0xfff write via its power-state chain, hours after
	 * boot — while Linux fires the genpd power-on at ~1.9 s, possibly before
	 * the PMC applies the rails, leaving the block half-alive (readable but
	 * never valid).  This stage re-cycles AVD_SYS ps off/on through the
	 * exact apple_pmgr_ps_set register choreography to re-kick the power
	 * at a later, settled time.  Runs before the macos-timing sleep. */
	if (preinit_mask & AVD_PREINIT_PSKICK) {
		void __iomem *ps;
		u32 reg, cur;
		int i;

		dev_info(avd->dev, "AVDBG preinit: stage 8 ps-kick: AVD_SYS ps off/on\n");
		ps = ioremap(AVD_PMGR_BASE_PHYS + 0x410, 4);
		if (ps) {
			reg = readl(ps);
			dev_info(avd->dev, "AVDBG pskick: initial ps=%08x\n", reg);
			/* off: clear DEV_DISABLE|PS_RESET|AUTO_ENABLE|FLAGS|TARGET
			 * (exact apple_pmgr_ps_set clear mask; bit31 GO and the
			 * PS_ACTUAL status nibble are left untouched) */
			pr_emerg("AVDBGX PK1: writing ps target 0 (off)\n");
			reg &= ~0x1000140f;
			writel(reg, ps);
			pr_emerg("AVDBGX PK2: off write returned, polling actual\n");
			for (i = 0; i < 100; i++) {
				cur = readl(ps);
				if ((cur & 0xf0) == 0x0)
					break;
				udelay(10);
			}
			dev_info(avd->dev, "AVDBG pskick: off -> ps=%08x (actual %x)\n",
				 cur, (cur >> 4) & 0xf);
			pr_emerg("AVDBGX PK3: sleeping 50 ms\n");
			msleep(50);
			/* on: same clear + target 0xf, then AUTO_ENABLE */
			reg = readl(ps);
			reg &= ~0x1000140f;
			reg |= 0xf;
			pr_emerg("AVDBGX PK4: writing ps target 0xf (on)\n");
			writel(reg, ps);
			pr_emerg("AVDBGX PK5: on write returned, polling actual\n");
			for (i = 0; i < 100; i++) {
				cur = readl(ps);
				if (((cur >> 4) & 0xf) == 0xf)
					break;
				udelay(10);
			}
			reg = readl(ps);
			reg |= 0x10000000; /* AUTO_ENABLE */
			writel(reg, ps);
			dev_info(avd->dev, "AVDBG pskick: on  -> ps=%08x (actual %x)\n",
				 readl(ps), (readl(ps) >> 4) & 0xf);
			pr_emerg("AVDBGX PK6: ps-kick done\n");
			iounmap(ps);
		}
	}

	/* macOS AppleAVD::setPowerStateOn keeps a ~726 ms window between the
	 * power-on chain and the fw upload (0x8c90748(0x2b680128,...) site,
	 * AVD_POWER_RE.md §5 erratum 5; the helper doubles as a kdebug trace
	 * site, so treat 726 ms as "macOS waits a while here" — AVD_CLOCKGATE_
	 * LINUX.md §6.1.3).  j293's FLAG0 never rises with the immediate
	 * upload; give the block the same settle time.  Runs last so the
	 * sleep lands immediately before avd_boot() uploads the fw. */
	if (preinit_mask & AVD_PREINIT_TIMING) {
		dev_info(avd->dev,
			 "AVDBG preinit: stage 7/7 macos-timing: sleep 726 ms before fw upload\n");
		msleep(726);
	}
}

int avd_boot(struct avd_dev *avd)
{
	u32 val;
	int ret;

	if (avd->variant->revision != 3)
		dev_info_once(avd->dev, "booting hw version: %04x",
				readl_relaxed(avd->ctrl));

	/* Full t8103 bring-up: ADS power, DART, wrap ctrl, DMA tunables, each
	 * stage gated by the apple_avd.preinit_mask kernel parameter (default
	 * 0 = skip everything, avd4-equivalent).
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
	pr_emerg("AVDBGX T1: RUN_CTRL set (freeze after this = poll phase)\n");

	/* macos-timing (bit 7): CAvdApCommViola::waitValidADSStatus poll.
	 * macOS samples ctrl+0x1002010 for (v & 0x7f0) == 0x7f0 after fw
	 * start (10 ms x 500, ~5 s) and observes 0x0 -> 0x7f0 ~400 ms after
	 * start even without fw upload — so on healthy hardware these bits
	 * rise on their own.  Diagnostic only: tells us whether j293's ADS
	 * block ever becomes valid, i.e. CM3-alive-but-slow vs never-started.
	 * NOTE: placed after RUN (macOS ordering) — reading ADS before its
	 * power domain is up faults (see stage 1 comment above). */
	if (avd->variant->revision == 3 && avd->full &&
	    (preinit_mask & AVD_PREINIT_TIMING)) {
		u32 st = readl(avd->full + AVD_OFF_ADS_STATUS);
		int i;

		dev_info(avd->dev, "AVDBG boot: ads-valid initial status=%08x\n", st);
		for (i = 0; i < 50; i++) {
			if ((st & 0x7f0) == 0x7f0)
				break;
			msleep(10);
			st = readl(avd->full + AVD_OFF_ADS_STATUS);
		}
		dev_info(avd->dev,
			 "AVDBG boot: ads-valid after ~%d ms status=%08x %s\n",
			 i * 10, st, (st & 0x7f0) == 0x7f0 ? "VALID" : "NOT-VALID");
	}

	/* wait for cm3 to boot.  macos-timing (bit 7) extends the window to
	 * 5 s (matching the macOS ADS poll patience) in case j293's CM3 is
	 * just slow; default stays at the j274-proven 10 ms. */
	if (preinit_mask & AVD_PREINIT_TIMING)
		ret = readl_poll_timeout(avd->mbox + AVD_REG_FLAG0_SET,
				val, val == 1, 10000, 5000000);
	else
		ret = readl_poll_timeout(avd->mbox + AVD_REG_FLAG0_SET,
				val, val == 1, 10, 10000);
	if (ret) {
		dev_info(avd->dev, "AVDBG boot: TIMEOUT flag0=%08x mbox=%08x\n",
			 readl_relaxed(avd->mbox + AVD_REG_FLAG0_SET),
			 readl_relaxed(avd->mbox + AVD_REG_MBOX1_RETRIEVE));

		/* ads-postrun (bit 8): the ADS power write (0x269000000 <- 0xfff)
		 * SError-panics BEFORE fw upload on j293 (avd5/6/7), so it has
		 * never been tried after RUN_CTRL.  t0 (avd10 mask=0x80) proved
		 * ADS reads are safe post-RUN and the block stays unpowered
		 * (status 0x0, never valid) — j274/m1n1 power it with exactly
		 * this write.  Try it now, then re-poll ADS-valid and FLAG0.
		 * If the write still faults, the freeze comes after all 3 GRUB
		 * markers and the last dmesg line localizes it. */
		if ((preinit_mask & AVD_PREINIT_ADS_POSTRUN) &&
		    avd->variant->revision == 3 && avd->full) {
			u32 st;
			int i;

			pr_emerg("AVDBGX ADS1: writing 0x1000000 <- 0xfff (post-RUN)\n");
			writel(0xfff, avd->full + AVD_OFF_ADS_PWR);
			pr_emerg("AVDBGX ADS2: write returned\n");
			dev_info(avd->dev, "AVDBG boot: ads-postrun: pwr readback=%08x\n",
				 readl(avd->full + AVD_OFF_ADS_PWR));

			st = readl(avd->full + AVD_OFF_ADS_STATUS);
			for (i = 0; i < 50; i++) {
				if ((st & 0x7f0) == 0x7f0)
					break;
				msleep(10);
				st = readl(avd->full + AVD_OFF_ADS_STATUS);
			}
			dev_info(avd->dev,
				 "AVDBG boot: ads-postrun: ads-valid after ~%d ms status=%08x %s\n",
				 i * 10, st, (st & 0x7f0) == 0x7f0 ? "VALID" : "NOT-VALID");

			pr_emerg("AVDBGX ADS3: re-polling FLAG0 5 s\n");
			ret = readl_poll_timeout(avd->mbox + AVD_REG_FLAG0_SET,
					val, val == 1, 10000, 5000000);
			if (!ret) {
				dev_info(avd->dev, "AVDBG boot: OK (ads-postrun)\n");
				return 0;
			}
			dev_info(avd->dev,
				 "AVDBG boot: ads-postrun: FLAG0 still 0 (%08x) after retry\n",
				 readl_relaxed(avd->mbox + AVD_REG_FLAG0_SET));
		}
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
