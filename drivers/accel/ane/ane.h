// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#ifndef __ANE_H__
#define __ANE_H__

#include <drm/drm_device.h>
#include <drm/drm_mm.h>

#include "uapi/drm/ane_accel.h"

#include <linux/device.h>

struct ane_device {
	struct drm_device drm;
	struct device *dev;
	const struct ane_hw *hw;

	struct device *pm_domains[5];
	struct device_link *pm_links[5];

	void __iomem *engine;
	void __iomem *dart0;
	void __iomem *dart1;
	void __iomem *dart2;

	struct drm_mm mm;
	struct iommu_domain *domain;
	unsigned long shift;

	int irq;
	int dart_irq;

	struct mutex iommu_lock;
	struct mutex engine_lock;
};

struct ane_hw {
	struct {
		u64 vm_base;
		u64 vm_size;
		u32 ttbr;
		u32 select;
		u32 command;
		u32 invalidate;
	} dart;
};

struct ane_request {
	int qid;
	u32 nid;
	u32 td_size;
	u32 td_count;
	u32 btsp_iova;
	u32 bar[ANE_TILE_COUNT];
};

#endif /* __ANE_H__ */
