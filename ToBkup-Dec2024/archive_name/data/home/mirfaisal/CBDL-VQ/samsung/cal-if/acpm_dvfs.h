/* SPDX-License-Identifier: GPL-2.0 */
/*
 * EXYNOS - CAL-IF support
 *
 * Copyright (c) 2020 Samsung Electronics Co., Ltd.
 *        http://www.samsung.com/
 */

#ifndef __ACPM_DVFS_H__
#define __ACPM_DVFS_H__

#include <linux/of.h>

struct acpm_dvfs {
	struct device_node *dev;
	unsigned int ch_num;
	unsigned int size;
};

#define FREQ_REQ        0
#define FREQ_GET        1
#define MARGIN_REQ      2
#define COLDTEMP_REQ    3
#define POLICY_REQ      4

#define SET_INIT_FREQ	3

#if IS_ENABLED(CONFIG_ACPM_DVFS)
int exynos_acpm_dvfs_init(void);
extern int exynos_acpm_set_rate(unsigned int id, unsigned long rate);
extern unsigned long exynos_acpm_get_rate(unsigned int id);
extern void exynos_acpm_set_device(void *dev);
extern int exynos_acpm_set_volt_margin(unsigned int id, int volt);
extern int exynos_acpm_set_cold_temp(unsigned int id, bool is_cold_temp);
#else
static int exynos_acpm_dvfs_init(void)
{
	return 0;
}
static inline int exynos_acpm_set_rate(unsigned int id, unsigned long rate)
{
	return 0;
}

static inline unsigned long exynos_acpm_get_rate(unsigned int id)
{
	return 0UL;
}

static inline void exynos_acpm_set_device(void *dev)
{
	return ;
}
static inline int exynos_acpm_set_volt_margin(unsigned int id, int volt)
{
	return 0;
}

static inline int exynos_acpm_set_cold_temp(unsigned int id, bool is_cold_temp)
{
	return 0;
}
#endif
#endif
