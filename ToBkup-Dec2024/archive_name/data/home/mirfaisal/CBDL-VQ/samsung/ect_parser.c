// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Samsung Electronics Co., Ltd.
 *               http://www.samsung.com
 */

#include <soc/samsung/ect_parser.h>

#include <asm/uaccess.h>
#include <asm/memory.h>
#include <asm/io.h>

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/string.h>
#include <linux/soc/samsung/exynos-soc.h>

#define ALIGNMENT_SIZE	 4

#define ARRAY_SIZE32(array)		((u32)ARRAY_SIZE(array))

#define NAME_MAX_LEN	20
/* Variable */

static struct ect_info ect_list[];

static char ect_signature[] = "PARA";

static struct class *ect_class;

static phys_addr_t ect_address;
static phys_addr_t ect_size;

static struct vm_struct ect_early_vm;

/* API for internal */

static void ect_parse_integer(void **address, void *value)
{
	*((unsigned int *)value) = __raw_readl(*address);
	*address += sizeof(uint32_t);
}

static void ect_parse_integer64(void **address, void *value)
{
	unsigned int top, half;

	half = __raw_readl(*address);
	*address += sizeof(uint32_t);
	top = __raw_readl(*address);
	*address += sizeof(uint32_t);

       *(unsigned long long *)value = ((unsigned long long)top << 32 | half);
}

static int ect_parse_string(void **address, char **value, unsigned int *length)
{
	ect_parse_integer(address, length);
	(*length)++;

	*value = *address;

	if (*length % ALIGNMENT_SIZE != 0)
		*address += (unsigned long)(*length + ALIGNMENT_SIZE - (*length % ALIGNMENT_SIZE));
	else
		*address += (unsigned long)*length;

	return 0;
}

static int ect_parse_dvfs_domain(int parser_version, void *address, struct ect_dvfs_domain *domain)
{
	int ret = 0;
	int i;
	char *clock_name;
	int length;

	ect_parse_integer(&address, &domain->max_frequency);
	ect_parse_integer(&address, &domain->min_frequency);

	if (parser_version >= 2) {
		ect_parse_integer(&address, &domain->boot_level_idx);
		ect_parse_integer(&address, &domain->resume_level_idx);
	} else {
		domain->boot_level_idx = -1;
		domain->resume_level_idx = -1;
	}

	if (parser_version >= 3) {
		ect_parse_integer(&address, &domain->mode);
	} else {
		domain->mode = e_dvfs_mode_clock_name;
	}

	ect_parse_integer(&address, &domain->num_of_clock);
	ect_parse_integer(&address, &domain->num_of_level);

	if (domain->mode == e_dvfs_mode_sfr_address) {
		domain->list_sfr = address;
		domain->list_clock = NULL;

		address += sizeof(unsigned int) * domain->num_of_clock;
	} else if (domain->mode == e_dvfs_mode_clock_name) {
		domain->list_clock = kzalloc(sizeof(char *) * domain->num_of_clock, GFP_KERNEL);
		domain->list_sfr = NULL;
		if (domain->list_clock == NULL) {
			ret = -ENOMEM;
			goto err_list_clock_allocation;
		}

		for (i = 0; i < domain->num_of_clock; ++i) {
			if (ect_parse_string(&address, &clock_name, &length)) {
				ret = -EINVAL;
				goto err_parse_string;
			}

			domain->list_clock[i] = clock_name;
		}
	}

	domain->list_level = address;
	address += sizeof(struct ect_dvfs_level) * domain->num_of_level;

	domain->list_dvfs_value = address;

	return 0;

err_parse_string:
	kfree(domain->list_clock);
err_list_clock_allocation:
	return ret;
}

/**
 * @cnotice
 * @prdcode
 * @unit_name{Parser_tool}
 * @purpose "To parse dvfs data"
 * @logic "parse basic and detail information"
 * @params
 * @param{in, address, void *, >=0}
 * @param{in, info, struct::ect_info, -}
 * @endparam
 * @retval{ret, int, 0, 0, -12}
 */
static int ect_parse_dvfs_header(void *address, struct ect_info *info)
{
	int ret = 0;
	int i;
	char *domain_name;
	unsigned int length, offset;
	struct ect_dvfs_header *ect_dvfs_header;
	struct ect_dvfs_domain *ect_dvfs_domain;
	void *address_dvfs_header = address;

	if (address == NULL)
		return -EINVAL;

	ect_dvfs_header = kzalloc(sizeof(struct ect_dvfs_header), GFP_KERNEL);
	if (ect_dvfs_header == NULL)
		return -ENOMEM;

	ect_parse_integer(&address, &ect_dvfs_header->parser_version);
	ect_parse_integer(&address, &ect_dvfs_header->version);
	ect_parse_integer(&address, &ect_dvfs_header->num_of_domain);

	ect_dvfs_header->domain_list = kzalloc(sizeof(struct ect_dvfs_domain) * ect_dvfs_header->num_of_domain,
						GFP_KERNEL);
	if (ect_dvfs_header->domain_list == NULL) {
		ret = -EINVAL;
		goto err_domain_list_allocation;
	}

	for (i = 0; i < ect_dvfs_header->num_of_domain; ++i) {
		if (ect_parse_string(&address, &domain_name, &length)) {
			ret = -EINVAL;
			goto err_parse_string;
		}

		ect_parse_integer(&address, &offset);

		ect_dvfs_domain = &ect_dvfs_header->domain_list[i];
		ect_dvfs_domain->domain_name = domain_name;
		ect_dvfs_domain->domain_offset = offset;
	}

	for (i = 0; i < ect_dvfs_header->num_of_domain; ++i) {
		ect_dvfs_domain = &ect_dvfs_header->domain_list[i];

		if (ect_parse_dvfs_domain(ect_dvfs_header->parser_version,
						address_dvfs_header + ect_dvfs_domain->domain_offset,
						ect_dvfs_domain)) {
			ret = -EINVAL;
			goto err_parse_domain;
		}
	}

	info->block_handle = ect_dvfs_header;

	return 0;

err_parse_domain:
err_parse_string:
	kfree(ect_dvfs_header->domain_list);
err_domain_list_allocation:
	kfree(ect_dvfs_header);
	return ret;
}

static int ect_parse_pll(int parser_version, void *address, struct ect_pll *ect_pll)
{
	ect_parse_integer(&address, &ect_pll->type_pll);
	ect_parse_integer(&address, &ect_pll->num_of_frequency);

	ect_pll->frequency_list = address;

	return 0;
}

/**
 * @cnotice
 * @prdcode
 * @unit_name{Parser_tool}
 * @purpose "To parse pll data"
 * @logic "parse basic and detail information"
 * @params
 * @param{in, address, void *, >=0}
 * @param{in, info, struct::ect_info, -}
 * @endparam
 * @retval{ret, int, 0, 0, -12}
 */
static int ect_parse_pll_header(void *address, struct ect_info *info)
{
	int ret = 0;
	int i;
	char *pll_name;
	unsigned int length, offset;
	struct ect_pll_header *ect_pll_header;
	struct ect_pll *ect_pll;
	void *address_pll_header = address;

	if (address == NULL)
		return -EINVAL;

	ect_pll_header = kzalloc(sizeof(struct ect_pll_header), GFP_KERNEL);
	if (ect_pll_header == NULL)
		return -ENOMEM;

	ect_parse_integer(&address, &ect_pll_header->parser_version);
	ect_parse_integer(&address, &ect_pll_header->version);
	ect_parse_integer(&address, &ect_pll_header->num_of_pll);

	ect_pll_header->pll_list = kzalloc(sizeof(struct ect_pll) * ect_pll_header->num_of_pll,
							GFP_KERNEL);
	if (ect_pll_header->pll_list == NULL) {
		ret = -ENOMEM;
		goto err_pll_list_allocation;
	}

	for (i = 0; i < ect_pll_header->num_of_pll; ++i) {

		if (ect_parse_string(&address, &pll_name, &length)) {
			ret = -EINVAL;
			goto err_parse_string;
		}

		ect_parse_integer(&address, &offset);

		ect_pll = &ect_pll_header->pll_list[i];
		ect_pll->pll_name = pll_name;
		ect_pll->pll_offset = offset;
	}

	for (i = 0; i < ect_pll_header->num_of_pll; ++i) {
		ect_pll = &ect_pll_header->pll_list[i];

		if (ect_parse_pll(ect_pll_header->parser_version,
					address_pll_header + ect_pll->pll_offset, ect_pll)) {
			ret = -EINVAL;
			goto err_parse_pll;
		}
	}

	info->block_handle = ect_pll_header;

	return 0;

err_parse_pll:
err_parse_string:
	kfree(ect_pll_header->pll_list);
err_pll_list_allocation:
	kfree(ect_pll_header);
	return ret;
}

static int ect_parse_voltage_table(int parser_version, void **address, struct ect_voltage_domain *domain, struct ect_voltage_table *table)
{
	int num_of_data = domain->num_of_group * domain->num_of_level;

	ect_parse_integer(address, &table->table_version);

	if (parser_version >= 2) {
		ect_parse_integer(address, &table->boot_level_idx);
		ect_parse_integer(address, &table->resume_level_idx);

		table->level_en = *address;
		*address += sizeof(int32_t) * domain->num_of_level;
	} else {
		table->boot_level_idx = -1;
		table->resume_level_idx = -1;

		table->level_en = NULL;
	}

	if (parser_version >= 3) {
		table->voltages = NULL;

		table->voltages_step = *address;
		*address += sizeof(unsigned char) * num_of_data;
		table->volt_step = PMIC_VOLTAGE_STEP;

	} else {
		table->voltages = *address;
		*address += sizeof(int32_t) * num_of_data;

		table->voltages_step = NULL;
		table->volt_step = 0;
	}

	return 0;
}

static int ect_parse_voltage_domain(int parser_version, void *address, struct ect_voltage_domain *domain)
{
	int ret = 0;
	int i;

	ect_parse_integer(&address, &domain->num_of_group);
	ect_parse_integer(&address, &domain->num_of_level);
	ect_parse_integer(&address, &domain->num_of_table);

	domain->level_list = address;
	address += sizeof(int32_t) * domain->num_of_level;

	domain->table_list = kzalloc(sizeof(struct ect_voltage_table) * domain->num_of_table, GFP_KERNEL);
	if (domain->table_list == NULL) {
		ret = -ENOMEM;
		goto err_table_list_allocation;
	}

	for (i = 0; i < domain->num_of_table; ++i) {
		if (ect_parse_voltage_table(parser_version,
						&address,
						domain,
						&domain->table_list[i])) {
			ret = -EINVAL;
			goto err_parse_voltage_table;
		}
	}

	return 0;

err_parse_voltage_table:
	kfree(domain->table_list);
err_table_list_allocation:
	return ret;
}

/**
 * @cnotice
 * @prdcode
 * @unit_name{Parser_tool}
 * @purpose "To parse voltage data"
 * @logic "parse basic and detail information"
 * @params
 * @param{in, address, void *, >=0}
 * @param{in, info, struct::ect_info, -}
 * @endparam
 * @retval{ret, int, 0, 0, -12}
 */
static int ect_parse_voltage_header(void *address, struct ect_info *info)
{
	int ret = 0;
	int i;
	char *domain_name;
	unsigned int length, offset;
	struct ect_voltage_header *ect_voltage_header;
	struct ect_voltage_domain *ect_voltage_domain;
	void *address_voltage_header = address;

	if (address == NULL)
		return -EINVAL;

	ect_voltage_header = kzalloc(sizeof(struct ect_voltage_header), GFP_KERNEL);
	if (ect_voltage_header == NULL)
		return -EINVAL;

	ect_parse_integer(&address, &ect_voltage_header->parser_version);
	ect_parse_integer(&address, &ect_voltage_header->version);
	ect_parse_integer(&address, &ect_voltage_header->num_of_domain);

	ect_voltage_header->domain_list = kzalloc(sizeof(struct ect_voltage_domain) * ect_voltage_header->num_of_domain,
							GFP_KERNEL);
	if (ect_voltage_header->domain_list == NULL) {
		ret = -ENOMEM;
		goto err_domain_list_allocation;
	}

	for (i = 0; i < ect_voltage_header->num_of_domain; ++i) {
		if (ect_parse_string(&address, &domain_name, &length)) {
			ret = -EINVAL;
			goto err_parse_string;
		}

		ect_parse_integer(&address, &offset);

		ect_voltage_domain = &ect_voltage_header->domain_list[i];
		ect_voltage_domain->domain_name = domain_name;
		ect_voltage_domain->domain_offset = offset;
	}

	for (i = 0; i < ect_voltage_header->num_of_domain; ++i) {
		ect_voltage_domain = &ect_voltage_header->domain_list[i];

		if (ect_parse_voltage_domain(ect_voltage_header->parser_version,
						address_voltage_header + ect_voltage_domain->domain_offset,
						ect_voltage_domain)) {
			ret = -EINVAL;
			goto err_parse_voltage_domain;
		}
	}

	info->block_handle = ect_voltage_header;

	return 0;

err_parse_voltage_domain:
err_parse_string:
	kfree(ect_voltage_header->domain_list);
err_domain_list_allocation:
	kfree(ect_voltage_header);
	return ret;
}

static int ect_parse_ap_thermal_function(int parser_version, void *address, struct ect_ap_thermal_function *function)
{
	int i;
	struct ect_ap_thermal_range *range;

	ect_parse_integer(&address, &function->num_of_range);

	function->range_list = kzalloc(sizeof(struct ect_ap_thermal_range) * function->num_of_range, GFP_KERNEL);

	for (i = 0; i < function->num_of_range; ++i) {
		range = &function->range_list[i];

		ect_parse_integer(&address, &range->lower_bound_temperature);
		ect_parse_integer(&address, &range->upper_bound_temperature);
		ect_parse_integer(&address, &range->max_frequency);
		ect_parse_integer(&address, &range->sw_trip);
		ect_parse_integer(&address, &range->flag);
	}

	return 0;
}

/**
 * @cnotice
 * @prdcode
 * @unit_name{Parser_tool}
 * @purpose "To parse ap_thermal data"
 * @logic "parse basic and detail information"
 * @params
 * @param{in, address, void *, >=0}
 * @param{in, info, struct::ect_info, -}
 * @endparam
 * @retval{ret, int, 0, 0, -12}
 */
static int ect_parse_ap_thermal_header(void *address, struct ect_info *info)
{
	int ret = 0;
	int i;
	char *function_name;
	unsigned int length, offset;
	struct ect_ap_thermal_header *ect_ap_thermal_header;
	struct ect_ap_thermal_function *ect_ap_thermal_function;
	void *address_thermal_header = address;

	if (address == NULL)
		return -EINVAL;

	ect_ap_thermal_header = kzalloc(sizeof(struct ect_ap_thermal_header), GFP_KERNEL);
	if (ect_ap_thermal_header == NULL)
		return -EINVAL;

	ect_parse_integer(&address, &ect_ap_thermal_header->parser_version);
	ect_parse_integer(&address, &ect_ap_thermal_header->version);
	ect_parse_integer(&address, &ect_ap_thermal_header->num_of_function);

	ect_ap_thermal_header->function_list = kzalloc(sizeof(struct ect_ap_thermal_function) * ect_ap_thermal_header->num_of_function,
								GFP_KERNEL);
	if (ect_ap_thermal_header->function_list == NULL) {
		ret = -ENOMEM;
		goto err_function_list_allocation;
	}

	for (i = 0; i < ect_ap_thermal_header->num_of_function; ++i) {
		if (ect_parse_string(&address, &function_name, &length)) {
			ret = -EINVAL;
			goto err_parse_string;
		}

		ect_parse_integer(&address, &offset);

		ect_ap_thermal_function = &ect_ap_thermal_header->function_list[i];
		ect_ap_thermal_function->function_name = function_name;
		ect_ap_thermal_function->function_offset = offset;
	}

	for (i = 0; i < ect_ap_thermal_header->num_of_function; ++i) {
		ect_ap_thermal_function = &ect_ap_thermal_header->function_list[i];

		if (ect_parse_ap_thermal_function(ect_ap_thermal_header->parser_version,
							address_thermal_header + ect_ap_thermal_function->function_offset,
							ect_ap_thermal_function)) {
			ret = -EINVAL;
			goto err_parse_ap_thermal_function;
		}
	}

	info->block_handle = ect_ap_thermal_header;

	return 0;

err_parse_ap_thermal_function:
err_parse_string:
	kfree(ect_ap_thermal_header->function_list);
err_function_list_allocation:
	kfree(ect_ap_thermal_header);
	return ret;
}

static int ect_parse_margin_domain(int parser_version, void *address, struct ect_margin_domain *domain)
{
	ect_parse_integer(&address, &domain->num_of_group);
	ect_parse_integer(&address, &domain->num_of_level);

	if (parser_version >= 2) {
		domain->offset = NULL;
		domain->offset_compact = address;
		domain->volt_step = PMIC_VOLTAGE_STEP;
	} else {
		domain->offset = address;
		domain->offset_compact = NULL;
	}

	return 0;
}

static int ect_parse_margin_header(void *address, struct ect_info *info)
{
	int ret = 0;
	int i;
	char *domain_name;
	unsigned int length, offset;
	struct ect_margin_header *ect_margin_header;
	struct ect_margin_domain *ect_margin_domain;
	void *address_margin_header = address;

	if (address == NULL)
		return -EINVAL;

	ect_margin_header = kzalloc(sizeof(struct ect_margin_header), GFP_KERNEL);
	if (ect_margin_header == NULL)
		return -EINVAL;

	ect_parse_integer(&address, &ect_margin_header->parser_version);
	ect_parse_integer(&address, &ect_margin_header->version);
	ect_parse_integer(&address, &ect_margin_header->num_of_domain);

	ect_margin_header->domain_list = kzalloc(sizeof(struct ect_margin_domain) * ect_margin_header->num_of_domain,
								GFP_KERNEL);
	if (ect_margin_header->domain_list == NULL) {
		ret = -ENOMEM;
		goto err_domain_list_allocation;
	}

	for (i = 0; i < ect_margin_header->num_of_domain; ++i) {
		if (ect_parse_string(&address, &domain_name, &length)) {
			ret = -EINVAL;
			goto err_parse_string;
		}

		ect_parse_integer(&address, &offset);

		ect_margin_domain = &ect_margin_header->domain_list[i];
		ect_margin_domain->domain_name = domain_name;
		ect_margin_domain->domain_offset = offset;
	}

	for (i = 0; i < ect_margin_header->num_of_domain; ++i) {
		ect_margin_domain = &ect_margin_header->domain_list[i];

		if (ect_parse_margin_domain(ect_margin_header->parser_version,
						address_margin_header + ect_margin_domain->domain_offset,
						ect_margin_domain)) {
			ret = -EINVAL;
			goto err_parse_margin_domain;
		}
	}

	info->block_handle = ect_margin_header;

	return 0;

err_parse_margin_domain:
err_parse_string:
	kfree(ect_margin_header->domain_list);
err_domain_list_allocation:
	kfree(ect_margin_header);
	return ret;
}

static int ect_parse_minlock_domain(int parser_version, void *address, struct ect_minlock_domain *domain)
{
	ect_parse_integer(&address, &domain->num_of_level);

	domain->level = address;

	return 0;
}

static int ect_parse_minlock_header(void *address, struct ect_info *info)
{
	int ret = 0;
	int i;
	char *domain_name;
	unsigned int length, offset;
	struct ect_minlock_header *ect_minlock_header;
	struct ect_minlock_domain *ect_minlock_domain;
	void *address_minlock_header = address;

	if (address == NULL)
		return -EINVAL;

	ect_minlock_header = kzalloc(sizeof(struct ect_minlock_header), GFP_KERNEL);
	if (ect_minlock_header == NULL)
		return -ENOMEM;

	ect_parse_integer(&address, &ect_minlock_header->parser_version);
	ect_parse_integer(&address, &ect_minlock_header->version);
	ect_parse_integer(&address, &ect_minlock_header->num_of_domain);

	ect_minlock_header->domain_list = kzalloc(sizeof(struct ect_minlock_domain) * ect_minlock_header->num_of_domain,
							GFP_KERNEL);
	if (ect_minlock_header->domain_list == NULL) {
		ret = -ENOMEM;
		goto err_domain_list_allocation;
	}

	for (i = 0; i < ect_minlock_header->num_of_domain; ++i) {
		if (ect_parse_string(&address, &domain_name, &length)) {
			ret = -EINVAL;
			goto err_parse_string;
		}

		ect_parse_integer(&address, &offset);

		ect_minlock_domain = &ect_minlock_header->domain_list[i];
		ect_minlock_domain->domain_name = domain_name;
		ect_minlock_domain->domain_offset = offset;
	}

	for (i = 0; i < ect_minlock_header->num_of_domain; ++i) {
		ect_minlock_domain = &ect_minlock_header->domain_list[i];

		if (ect_parse_minlock_domain(ect_minlock_header->parser_version,
					address_minlock_header + ect_minlock_domain->domain_offset,
					ect_minlock_domain)) {
			ret = -EINVAL;
			goto err_parse_minlock_domain;
		}
	}

	info->block_handle = ect_minlock_header;

	return 0;

err_parse_minlock_domain:
err_parse_string:
	kfree(ect_minlock_header->domain_list);
err_domain_list_allocation:
	kfree(ect_minlock_header);
	return ret;
}

static int ect_parse_gen_param_table(int parser_version, void *address, struct ect_gen_param_table *size)
{
	ect_parse_integer(&address, &size->num_of_col);
	ect_parse_integer(&address, &size->num_of_row);

	size->parameter = address;

	return 0;
}

/**
 * @cnotice
 * @prdcode
 * @unit_name{Parser_tool}
 * @purpose "To parse parse_gen_param data"
 * @logic "parse basic and detail information"
 * @params
 * @param{in, address, void *, >=0}
 * @param{in, info, struct::ect_info, -}
 * @endparam
 * @retval{ret, int, 0, 0, -12}
 */
static int ect_parse_gen_param_header(void *address, struct ect_info *info)
{
	int ret = 0;
	int i;
	char *table_name;
	unsigned int length, offset;
	struct ect_gen_param_header *ect_gen_param_header;
	struct ect_gen_param_table *ect_gen_param_table;
	void *address_param_header = address;

	if (address == NULL)
		return -EINVAL;

	ect_gen_param_header = kzalloc(sizeof(struct ect_gen_param_header), GFP_KERNEL);
	if (ect_gen_param_header == NULL)
		return -ENOMEM;

	ect_parse_integer(&address, &ect_gen_param_header->parser_version);
	ect_parse_integer(&address, &ect_gen_param_header->version);
	ect_parse_integer(&address, &ect_gen_param_header->num_of_table);

	ect_gen_param_header->table_list = kzalloc(sizeof(struct ect_gen_param_table) * ect_gen_param_header->num_of_table,
								GFP_KERNEL);
	if (ect_gen_param_header->table_list == NULL) {
		ret = -ENOMEM;
		goto err_table_list_allocation;
	}

	for (i = 0; i < ect_gen_param_header->num_of_table; ++i) {
		if (ect_parse_string(&address, &table_name, &length)) {
			ret = -EINVAL;
			goto err_parse_string;
		}

		ect_parse_integer(&address, &offset);

		ect_gen_param_table = &ect_gen_param_header->table_list[i];
		ect_gen_param_table->table_name = table_name;
		ect_gen_param_table->offset = offset;
	}

	for (i = 0; i < ect_gen_param_header->num_of_table; ++i) {
		ect_gen_param_table = &ect_gen_param_header->table_list[i];

		if (ect_parse_gen_param_table(ect_gen_param_header->parser_version,
							address_param_header + ect_gen_param_table->offset,
							ect_gen_param_table)) {
			ret = -EINVAL;
			goto err_parse_gen_param_table;
		}
	}

	info->block_handle = ect_gen_param_header;

	return 0;

err_parse_gen_param_table:
err_parse_string:
	kfree(ect_gen_param_header->table_list);
err_table_list_allocation:
	kfree(ect_gen_param_header);
	return ret;
}

static int ect_parse_new_timing_param_size(int parser_version, void *address, struct ect_new_timing_param_size *size)
{
	ect_parse_integer(&address, &size->mode);
	ect_parse_integer(&address, &size->num_of_timing_param);
	ect_parse_integer(&address, &size->num_of_level);

	size->timing_parameter = address;

	return 0;
}

/**
 * @cnotice
 * @prdcode
 * @unit_name{Parser_tool}
 * @purpose "To parse new_timing_param data"
 * @logic "parse basic and detail information"
 * @params
 * @param{in, address, void *, >=0}
 * @param{in, info, struct::ect_info, -}
 * @endparam
 * @retval{ret, int, 0, 0, -12}
 */
static int ect_parse_new_timing_param_header(void *address, struct ect_info *info)
{
	int ret = 0;
	int i;
	struct ect_new_timing_param_header *ect_new_timing_param_header;
	struct ect_new_timing_param_size *ect_new_timing_param_size;
	void *address_param_header = address;

	if (address == NULL)
		return -EINVAL;

	ect_new_timing_param_header = kzalloc(sizeof(struct ect_new_timing_param_header), GFP_KERNEL);
	if (ect_new_timing_param_header == NULL)
		return -ENOMEM;

	ect_parse_integer(&address, &ect_new_timing_param_header->parser_version);
	ect_parse_integer(&address, &ect_new_timing_param_header->version);
	ect_parse_integer(&address, &ect_new_timing_param_header->num_of_size);

	ect_new_timing_param_header->size_list = kzalloc(sizeof(struct ect_new_timing_param_size) * ect_new_timing_param_header->num_of_size,
								GFP_KERNEL);
	if (ect_new_timing_param_header->size_list == NULL) {
		ret = -ENOMEM;
		goto err_size_list_allocation;
	}

	for (i = 0; i < ect_new_timing_param_header->num_of_size; ++i) {
		ect_new_timing_param_size = &ect_new_timing_param_header->size_list[i];

		ect_parse_integer64(&address, &ect_new_timing_param_size->parameter_key);
		ect_parse_integer(&address, &ect_new_timing_param_size->offset);
	}

	for (i = 0; i < ect_new_timing_param_header->num_of_size; ++i) {
		ect_new_timing_param_size = &ect_new_timing_param_header->size_list[i];

		if (ect_parse_new_timing_param_size(ect_new_timing_param_header->parser_version,
							address_param_header + ect_new_timing_param_size->offset,
							ect_new_timing_param_size)) {
			ret = -EINVAL;
			goto err_parse_new_timing_param_size;
		}
	}

	info->block_handle = ect_new_timing_param_header;

	return 0;

err_parse_new_timing_param_size:
	kfree(ect_new_timing_param_header->size_list);
err_size_list_allocation:
	kfree(ect_new_timing_param_header);
	return ret;
}

static void ect_print_version(char *version)
{
	pr_info("ECT: Version %c%c%c%c\n", version[0], version[1], version[2], version[3]);
}

static int ect_dump_header(struct seq_file *s, void *data);
static int ect_dump_dvfs(struct seq_file *s, void *data);
static int ect_dump_pll(struct seq_file *s, void *data);
static int ect_dump_voltage(struct seq_file *s, void *data);
static int ect_dump_ap_thermal(struct seq_file *s, void *data);
static int ect_dump_margin(struct seq_file *s, void *data);
static int ect_dump_minlock(struct seq_file *s, void *data);
static int ect_dump_gen_parameter(struct seq_file *s, void *data);
static int ect_dump_new_timing_parameter(struct seq_file *s, void *data);

static int dump_open(struct inode *inode, struct file *file);

static struct ect_info ect_header_info = {
	.block_name = BLOCK_HEADER,
	.dump = ect_dump_header,
	.dump_ops = {
		.open = dump_open,
		.read = seq_read,
		.llseek = seq_lseek,
		.release = single_release,
	},
	.dump_node_name = SYSFS_NODE_HEADER,
	.block_handle = NULL,
	.block_precedence = -1,
};

static struct ect_info ect_list[] = {
	{
		.block_name = BLOCK_AP_THERMAL,
		.block_name_length = sizeof(BLOCK_AP_THERMAL) - 1,
		.parser = ect_parse_ap_thermal_header,
		.dump = ect_dump_ap_thermal,
		.dump_ops = {
			.open = dump_open,
			.read = seq_read,
			.llseek = seq_lseek,
			.release = single_release
		},
		.dump_node_name = SYSFS_NODE_AP_THERMAL,
		.block_handle = NULL,
		.block_precedence = -1,
	}, {
		.block_name = BLOCK_ASV,
		.block_name_length = sizeof(BLOCK_ASV) - 1,
		.parser = ect_parse_voltage_header,
		.dump = ect_dump_voltage,
		.dump_ops = {
			.open = dump_open,
			.read = seq_read,
			.llseek = seq_lseek,
			.release = single_release
		},
		.dump_node_name = SYSFS_NODE_ASV,
		.block_handle = NULL,
		.block_precedence = -1,
	}, {
		.block_name = BLOCK_DVFS,
		.block_name_length = sizeof(BLOCK_DVFS) - 1,
		.parser = ect_parse_dvfs_header,
		.dump = ect_dump_dvfs,
		.dump_ops = {
			.open = dump_open,
			.read = seq_read,
			.llseek = seq_lseek,
			.release = single_release
		},
		.dump_node_name = SYSFS_NODE_DVFS,
		.block_handle = NULL,
		.block_precedence = -1,
	}, {
		.block_name = BLOCK_MARGIN,
		.block_name_length = sizeof(BLOCK_MARGIN) - 1,
		.parser = ect_parse_margin_header,
		.dump = ect_dump_margin,
		.dump_ops = {
			.open = dump_open,
			.read = seq_read,
			.llseek = seq_lseek,
			.release = single_release
		},
		.dump_node_name = SYSFS_NODE_MARGIN,
		.block_handle = NULL,
		.block_precedence = -1,
	}, {
		.block_name = BLOCK_PLL,
		.block_name_length = sizeof(BLOCK_PLL) - 1,
		.parser = ect_parse_pll_header,
		.dump = ect_dump_pll,
		.dump_ops = {
			.open = dump_open,
			.read = seq_read,
			.llseek = seq_lseek,
			.release = single_release
		},
		.dump_node_name = SYSFS_NODE_PLL,
		.block_handle = NULL,
		.block_precedence = -1,
	}, {
		.block_name = BLOCK_MINLOCK,
		.block_name_length = sizeof(BLOCK_MINLOCK) - 1,
		.parser = ect_parse_minlock_header,
		.dump = ect_dump_minlock,
		.dump_ops = {
			.open = dump_open,
			.read = seq_read,
			.llseek = seq_lseek,
			.release = single_release
		},
		.dump_node_name = SYSFS_NODE_MINLOCK,
		.block_handle = NULL,
		.block_precedence = -1,
	}, {
		.block_name = BLOCK_GEN_PARAM,
		.block_name_length = sizeof(BLOCK_GEN_PARAM) - 1,
		.parser = ect_parse_gen_param_header,
		.dump = ect_dump_gen_parameter,
		.dump_ops = {
			.open = dump_open,
			.read = seq_read,
			.llseek = seq_lseek,
			.release = single_release
		},
		.dump_node_name = SYSFS_NODE_GEN_PARAM,
		.block_handle = NULL,
		.block_precedence = -1,
	}, {
		.block_name = BLOCK_NEW_TIMING_PARAM,
		.block_name_length = sizeof(BLOCK_NEW_TIMING_PARAM) - 1,
		.parser = ect_parse_new_timing_param_header,
		.dump = ect_dump_new_timing_parameter,
		.dump_ops = {
			.open = dump_open,
			.read = seq_read,
			.llseek = seq_lseek,
			.release = single_release,
		},
		.dump_node_name = SYSFS_NODE_NEW_TIMING_PARAM,
		.block_handle = NULL,
		.block_precedence = -1,
	}
};


static struct ect_info *ect_get_info(char *block_name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE32(ect_list); ++i) {
		if (ect_strcmp(block_name, ect_list[i].block_name) == 0)
			return &ect_list[i];
	}

	return NULL;
}

static int ect_dump_header(struct seq_file *s, void *data)
{
	struct ect_info *info = &ect_header_info;
	struct ect_header *header = info->block_handle;

	if (header == NULL) {
		seq_printf(s, "[ECT] : there is no ECT Information\n");
		return 0;
	}

	seq_printf(s, "[ECT] : ECT Information\n");
	seq_printf(s, "\t[VA] : %p\n", ect_early_vm.addr);
	seq_printf(s, "\t[SIGN] : %c%c%c%c\n",
			header->sign[0],
			header->sign[1],
			header->sign[2],
			header->sign[3]);
	seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
			header->version[0],
			header->version[1],
			header->version[2],
			header->version[3]);
	seq_printf(s, "\t[TOTAL SIZE] : %d\n", header->total_size);
	seq_printf(s, "\t[NUM OF HEADER] : %d\n", header->num_of_header);

	return 0;
}

static int ect_dump_dvfs(struct seq_file *s, void *data)
{
	int i, j, k;
	struct ect_dvfs_header *ect_dvfs_header;
	struct ect_dvfs_domain *domain;
	struct ect_info *info;

	info = ect_get_info(BLOCK_DVFS);
	if (info == NULL) {
		seq_puts(s, "info value is NULL\n");
		return 0;
	}

	ect_dvfs_header = info->block_handle;
	if (ect_dvfs_header == NULL) {
		seq_printf(s, "[ECT] : there is no dvfs information\n");
		return 0;
	}

	seq_printf(s, "[ECT] : DVFS Information\n");
	seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_dvfs_header->parser_version);
	seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
			ect_dvfs_header->version[0],
			ect_dvfs_header->version[1],
			ect_dvfs_header->version[2],
			ect_dvfs_header->version[3]);
	seq_printf(s, "\t[NUM OF DOMAIN] : %d\n", ect_dvfs_header->num_of_domain);

	for (i = 0; i < ect_dvfs_header->num_of_domain; ++i) {
		domain = &ect_dvfs_header->domain_list[i];

		seq_printf(s, "\t\t[DOMAIN NAME] : %s\n", domain->domain_name);
		seq_printf(s, "\t\t[BOOT LEVEL IDX] : ");
		if (domain->boot_level_idx == -1) {
			seq_printf(s, "NONE\n");
		} else {
			seq_printf(s, "%d\n", domain->boot_level_idx);
		}
		seq_printf(s, "\t\t[RESUME LEVEL IDX] : ");
		if (domain->resume_level_idx == -1) {
			seq_printf(s, "NONE\n");
		} else {
			seq_printf(s, "%d\n", domain->resume_level_idx);
		}
		seq_printf(s, "\t\t[MAX FREQ] : %u\n", domain->max_frequency);
		seq_printf(s, "\t\t[MIN FREQ] : %u\n", domain->min_frequency);
		if (domain->mode == e_dvfs_mode_clock_name) {
			seq_printf(s, "\t\t[NUM OF CLOCK] : %d\n", domain->num_of_clock);

			for (j = 0; j < domain->num_of_clock; ++j) {
				seq_printf(s, "\t\t\t[CLOCK NAME] : %s\n", domain->list_clock[j]);
			}
		} else if (domain->mode == e_dvfs_mode_sfr_address) {
			seq_printf(s, "\t\t[NUM OF SFR] : %d\n", domain->num_of_clock);

			for (j = 0; j < domain->num_of_clock; ++j) {
				seq_printf(s, "\t\t\t[SFR ADDRESS] : %x\n", domain->list_sfr[j]);
			}
		}

		seq_printf(s, "\t\t[NUM OF LEVEL] : %d\n", domain->num_of_level);

		for (j = 0; j < domain->num_of_level; ++j) {
			seq_printf(s, "\t\t\t[LEVEL] : %u(%c)\n",
					domain->list_level[j].level,
					domain->list_level[j].level_en ? 'O' : 'X');
		}

		seq_printf(s, "\t\t\t\t[TABLE]\n");
		for (j = 0; j < domain->num_of_level; ++j) {
			seq_printf(s, "\t\t\t\t");
			for (k = 0; k < domain->num_of_clock; ++k) {
				seq_printf(s, "%u ", domain->list_dvfs_value[j * domain->num_of_clock + k]);
			}
			seq_printf(s, "\n");
		}
	}

	return 0;
}

static int ect_dump_pll(struct seq_file *s, void *data)
{
	int i, j;
	struct ect_pll_header *ect_pll_header;
	struct ect_pll *pll;
	struct ect_pll_frequency *frequency;
	struct ect_info *info;

	info = ect_get_info(BLOCK_PLL);
	if (info == NULL) {
		seq_puts(s, "info value is NULL\n");
		return 0;
	}

	ect_pll_header = info->block_handle;
	if (ect_pll_header == NULL) {
		seq_printf(s, "[ECT] : there is no pll information\n");
		return 0;
	}

	seq_printf(s, "[ECT] : PLL Information\n");
	seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_pll_header->parser_version);
	seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
			ect_pll_header->version[0],
			ect_pll_header->version[1],
			ect_pll_header->version[2],
			ect_pll_header->version[3]);
	seq_printf(s, "\t[NUM OF PLL] : %d\n", ect_pll_header->num_of_pll);

	for (i = 0; i < ect_pll_header->num_of_pll; ++i) {
		pll = &ect_pll_header->pll_list[i];

		seq_printf(s, "\t\t[PLL NAME] : %s\n", pll->pll_name);
		seq_printf(s, "\t\t[PLL TYPE] : %d\n", pll->type_pll);
		seq_printf(s, "\t\t[NUM OF FREQUENCY] : %d\n", pll->num_of_frequency);

		for (j = 0; j < pll->num_of_frequency; ++j) {
			frequency = &pll->frequency_list[j];

			seq_printf(s, "\t\t\t[FREQUENCY] : %u\n", frequency->frequency);
			seq_printf(s, "\t\t\t[P] : %d\n", frequency->p);
			seq_printf(s, "\t\t\t[M] : %d\n", frequency->m);
			seq_printf(s, "\t\t\t[S] : %d\n", frequency->s);
			seq_printf(s, "\t\t\t[K] : %d\n", frequency->k);
		}
	}

	return 0;
}

static int ect_dump_voltage(struct seq_file *s, void *data)
{
	int i, j, k, l;
	struct ect_voltage_header *ect_voltage_header;
	struct ect_voltage_domain *domain;
	struct ect_info *info;

	info = ect_get_info(BLOCK_ASV);
	if (info == NULL) {
		seq_puts(s, "info value is NULL\n");
		return 0;
	}

	ect_voltage_header = info->block_handle;
	if (ect_voltage_header == NULL) {
		seq_printf(s, "[ECT] : there is no asv information\n");
		return 0;
	}

	seq_printf(s, "[ECT] : ASV Voltage Information\n");
	seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_voltage_header->parser_version);
	seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
			ect_voltage_header->version[0],
			ect_voltage_header->version[1],
			ect_voltage_header->version[2],
			ect_voltage_header->version[3]);
	seq_printf(s, "\t[NUM OF DOMAIN] : %d\n", ect_voltage_header->num_of_domain);

	for (i = 0; i < ect_voltage_header->num_of_domain; ++i) {
		domain = &ect_voltage_header->domain_list[i];

		seq_printf(s, "\t\t[DOMAIN NAME] : %s\n", domain->domain_name);
		seq_printf(s, "\t\t[NUM OF ASV GROUP] : %d\n", domain->num_of_group);
		seq_printf(s, "\t\t[NUM OF LEVEL] : %d\n", domain->num_of_level);

		for (j = 0; j < domain->num_of_level; ++j) {
			seq_printf(s, "\t\t\t[FREQUENCY] : %u\n", domain->level_list[j]);
		}

		seq_printf(s, "\t\t[NUM OF TABLE] : %d\n", domain->num_of_table);

		for (j = 0; j < domain->num_of_table; ++j) {
			seq_printf(s, "\t\t\t[TABLE VERSION] : %d\n", domain->table_list[j].table_version);
			seq_printf(s, "\t\t\t[BOOT LEVEL IDX] : ");
			if (domain->table_list[j].boot_level_idx == -1) {
				seq_printf(s, "NONE\n");
			} else {
				seq_printf(s, "%d\n", domain->table_list[j].boot_level_idx);
			}
			seq_printf(s, "\t\t\t[RESUME LEVEL IDX] : ");
			if (domain->table_list[j].resume_level_idx == -1) {
				seq_printf(s, "NONE\n");
			} else {
				seq_printf(s, "%d\n", domain->table_list[j].resume_level_idx);
			}
			seq_printf(s, "\t\t\t\t[TABLE]\n");
			for (k = 0; k < domain->num_of_level; ++k) {
				seq_printf(s, "\t\t\t\t");
				for (l = 0; l < domain->num_of_group; ++l) {
					if (domain->table_list[j].voltages != NULL)
						seq_printf(s, "%u ", domain->table_list[j].voltages[k * domain->num_of_group + l]);
					else if (domain->table_list[j].voltages_step != NULL)
						seq_printf(s, "%u ", domain->table_list[j].voltages_step[k * domain->num_of_group + l]
									* domain->table_list[j].volt_step);
				}
				seq_printf(s, "\n");
			}
		}
	}

	return 0;
}

static int ect_dump_ap_thermal(struct seq_file *s, void *data)
{
	int i, j;
	struct ect_ap_thermal_header *ect_ap_thermal_header;
	struct ect_ap_thermal_function *function;
	struct ect_ap_thermal_range *range;
	struct ect_info *info;

	info = ect_get_info(BLOCK_AP_THERMAL);
	if (info == NULL) {
		seq_puts(s, "info value is NULL\n");
		return 0;
	}

	ect_ap_thermal_header = info->block_handle;
	if (ect_ap_thermal_header == NULL) {
		seq_printf(s, "[ECT] : there is no ap thermal information\n");
		return 0;
	}

	seq_printf(s, "[ECT] : AP Thermal Information\n");
	seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_ap_thermal_header->parser_version);
	seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
			ect_ap_thermal_header->version[0],
			ect_ap_thermal_header->version[1],
			ect_ap_thermal_header->version[2],
			ect_ap_thermal_header->version[3]);
	seq_printf(s, "\t[NUM OF FUNCTION] : %d\n", ect_ap_thermal_header->num_of_function);

	for (i = 0; i < ect_ap_thermal_header->num_of_function; ++i) {
		function = &ect_ap_thermal_header->function_list[i];

		seq_printf(s, "\t\t[FUNCTION NAME] : %s\n", function->function_name);
		seq_printf(s, "\t\t[NUM OF RANGE] : %d\n", function->num_of_range);

		for (j = 0; j < function->num_of_range; ++j) {
			range = &function->range_list[j];

			seq_printf(s, "\t\t\t[LOWER BOUND TEMPERATURE] : %u\n", range->lower_bound_temperature);
			seq_printf(s, "\t\t\t[UPPER BOUND TEMPERATURE] : %u\n", range->upper_bound_temperature);
			seq_printf(s, "\t\t\t[MAX FREQUENCY] : %u\n", range->max_frequency);
			seq_printf(s, "\t\t\t[SW TRIP] : %u\n", range->sw_trip);
			seq_printf(s, "\t\t\t[FLAG] : %u\n", range->flag);
		}
	}

	return 0;
}

static int ect_dump_margin(struct seq_file *s, void *data)
{
	int i, j, k;
	struct ect_margin_header *ect_margin_header;
	struct ect_margin_domain *domain;
	struct ect_info *info;

	info = ect_get_info(BLOCK_MARGIN);
	if (info == NULL) {
		seq_puts(s, "info value is  NULL\n");
		return 0;
	}

	ect_margin_header = info->block_handle;
	if (ect_margin_header == NULL) {
		seq_printf(s, "[ECT] : there is no margin information\n");
		return 0;
	}

	seq_printf(s, "[ECT] : Margin Information\n");
	seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_margin_header->parser_version);
	seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
			ect_margin_header->version[0],
			ect_margin_header->version[1],
			ect_margin_header->version[2],
			ect_margin_header->version[3]);
	seq_printf(s, "\t[NUM OF DOMAIN] : %d\n", ect_margin_header->num_of_domain);

	for (i = 0; i < ect_margin_header->num_of_domain; ++i) {
		domain = &ect_margin_header->domain_list[i];

		seq_printf(s, "\t\t[DOMAIN NAME] : %s\n", domain->domain_name);
		seq_printf(s, "\t\t[NUM OF GROUP] : %d\n", domain->num_of_group);
		seq_printf(s, "\t\t[NUM OF LEVEL] : %d\n", domain->num_of_level);

		seq_printf(s, "\t\t\t[TABLE]\n");
		for (j = 0; j < domain->num_of_level; ++j) {
			seq_printf(s, "\t\t\t");
			for (k = 0; k < domain->num_of_group; ++k) {
				if (domain->offset != NULL)
					seq_printf(s, "%u ", domain->offset[j * domain->num_of_group + k]);
				else if (domain->offset_compact != NULL)
					seq_printf(s, "%u ", domain->offset_compact[j * domain->num_of_group + k]
								* domain->volt_step);
			}
			seq_printf(s, "\n");
		}
	}

	return 0;
}

static int ect_dump_minlock(struct seq_file *s, void *data)
{
	int i, j;
	struct ect_minlock_header *ect_minlock_header;
	struct ect_minlock_domain *domain;
	struct ect_info *info;

	info = ect_get_info(BLOCK_MINLOCK);
	if (info == NULL) {
		seq_puts(s, "info value is NULL\n");
		return 0;
	}

	ect_minlock_header = info->block_handle;
	if (ect_minlock_header == NULL) {
		seq_printf(s, "[ECT] : there is no minlock information\n");
		return 0;
	}

	seq_printf(s, "[ECT] : Minlock Information\n");
	seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_minlock_header->parser_version);
	seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
			ect_minlock_header->version[0],
			ect_minlock_header->version[1],
			ect_minlock_header->version[2],
			ect_minlock_header->version[3]);
	seq_printf(s, "\t[NUM OF DOMAIN] : %d\n", ect_minlock_header->num_of_domain);

	for (i = 0; i < ect_minlock_header->num_of_domain; ++i) {
		domain = &ect_minlock_header->domain_list[i];

		seq_printf(s, "\t\t[DOMAIN NAME] : %s\n", domain->domain_name);

		for (j = 0; j < domain->num_of_level; ++j) {
			seq_printf(s, "\t\t\t[Frequency] : (MAIN)%u, (SUB)%u\n",
					domain->level[j].main_frequencies,
					domain->level[j].sub_frequencies);
		}
	}

	return 0;
}

static int ect_dump_gen_parameter(struct seq_file *s, void *data)
{
	int i, j, k;
	struct ect_gen_param_header *ect_gen_param_header;
	struct ect_gen_param_table *table;
	struct ect_info *info;

	info = ect_get_info(BLOCK_GEN_PARAM);
	if (info == NULL) {
		seq_puts(s, "info value is NULL\n");
		return 0;
	}

	ect_gen_param_header = info->block_handle;
	if (ect_gen_param_header == NULL) {
		seq_printf(s, "[ECT] : there is no general parameter information\n");
		return 0;
	}

	seq_printf(s, "[ECT] : General-Parameter Information\n");
	seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_gen_param_header->parser_version);
	seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
			ect_gen_param_header->version[0],
			ect_gen_param_header->version[1],
			ect_gen_param_header->version[2],
			ect_gen_param_header->version[3]);
	seq_printf(s, "\t[NUM OF TABLE] : %d\n", ect_gen_param_header->num_of_table);

	for (i = 0; i < ect_gen_param_header->num_of_table; ++i) {
		table = &ect_gen_param_header->table_list[i];

		seq_printf(s, "\t\t[TABLE NAME] : %s\n", table->table_name);
		seq_printf(s, "\t\t[NUM OF COLUMN] : %d\n", table->num_of_col);
		seq_printf(s, "\t\t[NUM OF ROW] : %d\n", table->num_of_row);

		seq_printf(s, "\t\t\t[TABLE]\n");
		for (j = 0; j < table->num_of_row; ++j) {
			seq_printf(s, "\t\t\t");
			for (k = 0; k < table->num_of_col; ++k) {
				seq_printf(s, "%u ", table->parameter[j * table->num_of_col + k]);
			}
			seq_printf(s, "\n");
		}
	}

	return 0;
}

static int ect_dump_new_timing_parameter(struct seq_file *s, void *data)
{
	int i, j, k;
	struct ect_new_timing_param_header *ect_new_timing_param_header;
	struct ect_new_timing_param_size *size;
	struct ect_info *info;

	info = ect_get_info(BLOCK_NEW_TIMING_PARAM);
	if (info == NULL) {
		seq_puts(s, "info value is NULL\n");
		return 0;
	}

	ect_new_timing_param_header = info->block_handle;
	if (ect_new_timing_param_header == NULL) {
		seq_printf(s, "[ECT] : there is no new timing parameter information\n");
		return 0;
	}

	seq_printf(s, "[ECT] : New Timing-Parameter Information\n");
	seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_new_timing_param_header->parser_version);
	seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
			ect_new_timing_param_header->version[0],
			ect_new_timing_param_header->version[1],
			ect_new_timing_param_header->version[2],
			ect_new_timing_param_header->version[3]);
	seq_printf(s, "\t[NUM OF SIZE] : %d\n", ect_new_timing_param_header->num_of_size);

	for (i = 0; i < ect_new_timing_param_header->num_of_size; ++i) {
		size = &ect_new_timing_param_header->size_list[i];

		seq_printf(s, "\t\t[PARAMETER KEY] : %llX\n", size->parameter_key);
		seq_printf(s, "\t\t[NUM OF TIMING PARAMETER] : %d\n", size->num_of_timing_param);
		seq_printf(s, "\t\t[NUM OF LEVEL] : %d\n", size->num_of_level);

		seq_printf(s, "\t\t\t[TABLE]\n");
		for (j = 0; j < size->num_of_level; ++j) {
			seq_printf(s, "\t\t\t");
			for (k = 0; k < size->num_of_timing_param; ++k) {
				if (size->mode == e_mode_normal_value)
					seq_printf(s, "%X ", size->timing_parameter[j * size->num_of_timing_param + k]);
				else if (size->mode == e_mode_extend_value)
					seq_printf(s, "%llX ", ect_read_value64(size->timing_parameter, j * size->num_of_timing_param + k));
			}
			seq_printf(s, "\n");
		}
	}

	return 0;
}

static int dump_open(struct inode *inode, struct file *file)
{
	struct ect_info *info = (struct ect_info *)inode->i_private;

	return single_open(file, info->dump, inode->i_private);
}

static int ect_dump_all(struct seq_file *s, void *data)
{
	int i, j, ret;

	ret = ect_header_info.dump(s, data);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE32(ect_list); ++i) {
		for (j = 0; j < ARRAY_SIZE32(ect_list); ++j) {
			if (ect_list[j].block_precedence != i)
				continue;

			ret = ect_list[j].dump(s, data);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/**
 * @cnotice
 * @prdcode
 * @unit_name{Dump_tool}
 * @purpose "To interface file system to dump function"
 * @logic "file open"
 * @params
 * @param{in, inode, struct::inode, -}
 * @param{in, file, struct::file, -}
 * @endparam
 * @retval{-, -, -, 0, -12}
 */
static int dump_all_open(struct inode *inode, struct file *file)
{
	pr_info("ECT: Print dump file\n");
	return single_open(file, ect_dump_all, inode->i_private);
}

static struct file_operations ops_all_dump = {
	.open = dump_all_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**
 * @cnotice
 * @prdcode
 * @unit_name{Dump_init}
 * @purpose "create file system for showing debug information"
 * @logic "create ect node and allocate each dump file under ect node"
 * @noparam
 * @retval{ret, int, 0, 0, -12}
 */
static int ect_dump_init(void)
{
	int i;
	struct dentry *root, *d;

	root = debugfs_create_dir("ect", NULL);
	if (!root) {
		pr_err("%s: couln't create debugfs\n", __FILE__);
		return -ENOMEM;
	}

	d = debugfs_create_file("all_dump", S_IRUGO, root, NULL,
				&ops_all_dump);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file(ect_header_info.dump_node_name, S_IRUGO, root, &ect_header_info,
				&ect_header_info.dump_ops);
	if (!d)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE32(ect_list); ++i) {
		d = debugfs_create_file(ect_list[i].dump_node_name, S_IRUGO, root, &(ect_list[i]),
					&ect_list[i].dump_ops);
		if (!d)
			return -ENOMEM;
	}

	ect_class = class_create(THIS_MODULE, "ect");
	if (IS_ERR(ect_class)) {
		pr_err("%s: couldn't create class\n", __FILE__);
		return PTR_ERR(ect_class);
	}

	return 0;
}


/* API for external */


unsigned long long ect_read_value64(unsigned int *address, int index)
{
	unsigned int top, half;

	index *= 2;

	half = address[index];
	top = address[index + 1];

	return ((unsigned long long)top << 32 | half);
}

void *ect_get_block(char *block_name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE32(ect_list); ++i) {
		if (ect_strcmp(block_name, ect_list[i].block_name) == 0)
			return ect_list[i].block_handle;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ect_get_block);

struct ect_dvfs_domain *ect_dvfs_get_domain(void *block, char *domain_name)
{
	int i;
	struct ect_dvfs_header *header;
	struct ect_dvfs_domain *domain;
	char compare_name[NAME_MAX_LEN];

	if (block == NULL ||
		domain_name == NULL)
		return NULL;

	if (strlen(domain_name) >= NAME_MAX_LEN)
		return NULL;
	ect_rename(compare_name, domain_name);
	header = (struct ect_dvfs_header *)block;

	for (i = 0; i < header->num_of_domain; ++i) {
		domain = &header->domain_list[i];

		if (ect_strcmp(compare_name, domain->domain_name) == 0)
			return domain;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ect_dvfs_get_domain);

struct ect_pll *ect_pll_get_pll(void *block, char *pll_name)
{
	int i;
	struct ect_pll_header *header;
	struct ect_pll *pll;

	if (block == NULL ||
		pll_name == NULL)
		return NULL;

	header = (struct ect_pll_header *)block;

	for (i = 0; i < header->num_of_pll; ++i) {
		pll = &header->pll_list[i];

		if (strstr(pll_name, pll->pll_name) != NULL)
			return pll;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ect_pll_get_pll);

struct ect_voltage_domain *ect_asv_get_domain(void *block, char *domain_name)
{
	int i;
	struct ect_voltage_header *header;
	struct ect_voltage_domain *domain;
	char compare_name[NAME_MAX_LEN];

	if (block == NULL ||
		domain_name == NULL)
		return NULL;

	if (strlen(domain_name) >= NAME_MAX_LEN)
		return NULL;
	ect_rename(compare_name, domain_name);
	header = (struct ect_voltage_header *)block;

	for (i = 0; i < header->num_of_domain; ++i) {
		domain = &header->domain_list[i];

		if (ect_strcmp(compare_name, domain->domain_name) == 0)
			return domain;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ect_asv_get_domain);

struct ect_ap_thermal_function *ect_ap_thermal_get_function(void *block, char *function_name)
{
	int i;
	struct ect_ap_thermal_header *header;
	struct ect_ap_thermal_function *function;

	if (block == NULL ||
		function_name == NULL)
		return NULL;

	header = (struct ect_ap_thermal_header *)block;

	for (i = 0; i < header->num_of_function; ++i) {
		function = &header->function_list[i];

		if (ect_strcmp(function_name, function->function_name) == 0)
			return function;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ect_ap_thermal_get_function);

struct ect_margin_domain *ect_margin_get_domain(void *block, char *domain_name)
{
	int i;
	struct ect_margin_header *header;
	struct ect_margin_domain *domain;

	if (block == NULL ||
		domain_name == NULL)
		return NULL;

	header = (struct ect_margin_header *)block;

	for (i = 0; i < header->num_of_domain; ++i) {
		domain = &header->domain_list[i];

		if (ect_strcmp(domain_name, domain->domain_name) == 0)
			return domain;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ect_margin_get_domain);

struct ect_minlock_domain *ect_minlock_get_domain(void *block, char *domain_name)
{
	int i;
	struct ect_minlock_header *header;
	struct ect_minlock_domain *domain;

	if (block == NULL ||
		domain_name == NULL)
		return NULL;

	header = (struct ect_minlock_header *)block;

	for (i = 0; i < header->num_of_domain; ++i) {
		domain = &header->domain_list[i];

		if (ect_strcmp(domain_name, domain->domain_name) == 0)
			return domain;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ect_minlock_get_domain);

struct ect_gen_param_table *ect_gen_param_get_table(void *block, char *table_name)
{
	int i;
	struct ect_gen_param_header *header;
	struct ect_gen_param_table *table;

	if (block == NULL)
		return NULL;

	header = (struct ect_gen_param_header *)block;

	for (i = 0; i < header->num_of_table; ++i) {
		table = &header->table_list[i];

		if (ect_strcmp(table->table_name, table_name) == 0)
			return table;
		}

	 return NULL;
}
EXPORT_SYMBOL_GPL(ect_gen_param_get_table);

struct ect_new_timing_param_size *ect_new_timing_param_get_key(void *block, unsigned long long key)
{
	int i;
	struct ect_new_timing_param_header *header;
	struct ect_new_timing_param_size *size;

	if (block == NULL)
		return NULL;

	header = (struct ect_new_timing_param_header *)block;

	for (i = 0; i < header->num_of_size; ++i) {
		size = &header->size_list[i];

		if (key == size->parameter_key)
			return size;
	}

	return NULL;
}

/**
 * @cnotice
 * @prdcode
 * @unit_name{Parser_init}
 * @purpose "To parse ECT binary based on the category"
 * @logic "init map io, execute parse function loop for num_of_header count"
 * @params
 * @param{in, ect_address, phys_addr_t, >=0}
 * @retval{ret, int, 0, 0, -22}
 */
int ect_parse_binary_header(void)
{
	int ret = 0;
	int i, j;
	char *block_name;
	void *address;
	unsigned int length, offset;
	struct ect_header *ect_header;

	address = (void *)ect_address;
	if (address == NULL)
		return -EINVAL;

	ect_header = kzalloc(sizeof(struct ect_header), GFP_KERNEL);

	ect_parse_integer(&address, ect_header->sign);
	ect_parse_integer(&address, ect_header->version);
	ect_parse_integer(&address, &ect_header->total_size);
	ect_parse_integer(&address, &ect_header->num_of_header);

	if (memcmp(ect_header->sign, ect_signature, sizeof(ect_signature) - 1)) {
		ret = -EINVAL;
		goto err_memcmp;
	}

	ect_print_version(ect_header->version);

	for (i = 0; i < ect_header->num_of_header; ++i) {
		if (ect_parse_string(&address, &block_name, &length)) {
			ret = -EINVAL;
			goto err_parse_string;
		}

		pr_info("ECT: %s parsing\n", block_name);

		ect_parse_integer(&address, &offset);

		for (j = 0; j < ARRAY_SIZE32(ect_list); ++j) {
			if (strncmp(block_name, ect_list[j].block_name, ect_list[j].block_name_length) != 0)
				continue;

			if (ect_list[j].parser((void *)ect_address + offset, ect_list + j)) {
				pr_err("[ECT] : parse error %s\n", block_name);
				ret = -EINVAL;
				goto err_parser;
			}

			ect_list[j].block_precedence = i;
		}
	}

	pr_info("ECT: All ECT parameter parsing done.");
	ect_header_info.block_handle = ect_header;

	return ret;

err_parser:
err_parse_string:
err_memcmp:
	kfree(ect_header);

	return ret;
}
EXPORT_SYMBOL_GPL(ect_parse_binary_header);

int ect_strcmp(char *src1, char *src2)
{
	for ( ; *src1 == *src2; src1++, src2++)
		if (*src1 == '\0')
			return 0;

	return ((*(unsigned char *)src1 < *(unsigned char *)src2) ? -1 : 1);
}

int ect_strncmp(char *src1, char *src2, int length)
{
	int i;

	if (length <= 0)
		return -1;

	for (i = 0; i < length; i++, src1++, src2++)
		if (*src1 != *src2)
			return ((*(unsigned char *)src1 < *(unsigned char *)src2) ? -1 : 1);

	return 0;
}

void ect_rename(char *dest, char *src)
{
	strcpy(dest, src);
	if ((exynos_soc_info.product_id == EXYNOSAUTOV920_SOC_ID)
			&& (exynos_soc_info.main_rev == 0))
		strcat(dest, "_E0");
}

static void *exynos_ect_request_region(unsigned long addr,
					unsigned int size)
{
	int i;
	unsigned int num_pages = (size >> PAGE_SHIFT);
	pgprot_t prot = pgprot_writecombine(PAGE_KERNEL);
	struct page **pages = NULL;
	void *v_addr = NULL;

	if (!addr)
		return NULL;

	pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_ATOMIC);
	if (!pages)
		return NULL;

	for (i = 0; i < num_pages; i++) {
		pages[i] = phys_to_page(addr);
		addr += PAGE_SIZE;
	}

	v_addr = vmap(pages, num_pages, VM_MAP, prot);
	kfree(pages);

	return v_addr;
}

static int exynos_ect_probe(struct platform_device *pdev)
{
	struct reserved_mem *rmem;
	struct device_node *rmem_np;

	rmem_np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!rmem_np) {
		dev_err(&pdev->dev, "failed to acquire memory region node\n");
		return -ENODEV;
	} else {
		rmem = of_reserved_mem_lookup(rmem_np);
		if (!rmem) {
			dev_err(&pdev->dev, "failed to acquire memory region\n");
			return -ENOMEM;
		}
	}

	ect_early_vm.phys_addr = rmem->base;
	ect_early_vm.size = rmem->size;

	dev_info(&pdev->dev, "%s: Reserved memory for ect: addr=%llx, size=%lx\n",
			__func__, ect_early_vm.phys_addr, ect_early_vm.size);

	/* Translate PA to VA of message buffer */
	ect_early_vm.addr = exynos_ect_request_region(ect_early_vm.phys_addr, ect_early_vm.size);
	if (!ect_early_vm.addr) {
		dev_err(&pdev->dev, "Fail to translate message buffer\n");
		return -EFAULT;
	}

	ect_address = (phys_addr_t)ect_early_vm.addr;
	ect_size = ect_early_vm.size;

	dev_info(&pdev->dev, "Exynos ect driver probe done!\n");

	ect_dump_init();

	return 0;
}

static const struct of_device_id exynos_ect_of_match_table[] = {
	{ .compatible = "samsung,exynos-ect", },
	{ },
};
MODULE_DEVICE_TABLE(of, exynos_ect_of_match_table);

static struct platform_driver exynos_ect_driver = {
	.driver = {
		.name = "exynos-ect",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(exynos_ect_of_match_table),
	},
	.probe = exynos_ect_probe,
};

static int exynos_ect_init(void)
{
	return platform_driver_register(&exynos_ect_driver);
}

static void __exit exynos_ect_exit(void)
{
	platform_driver_unregister(&exynos_ect_driver);
}

postcore_initcall(exynos_ect_init);
module_exit(exynos_ect_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ect_parser module");
