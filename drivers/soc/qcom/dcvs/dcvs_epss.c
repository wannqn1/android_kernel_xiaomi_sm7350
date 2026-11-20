// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "qcom-dcvs-epss: " fmt

#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <soc/qcom/dcvs.h>
#include "dcvs_private.h"

struct epss_dev_data {
	void __iomem	*l3_base;
	resource_size_t	l3_size;
	u32		l3_shared_offset;
	u32		*l3_percpu_offsets;
};

#define MAX_L3_ENTRIES		40U
#define INIT_HZ			300000000UL
#define XO_HZ			19200000UL
#define FTBL_ROW_SIZE		4
#define SRC_MASK		GENMASK(31, 30)
#define MULT_MASK		GENMASK(7, 0)
int populate_l3_table(struct device *dev, u32 **freq_table)
{
	struct resource res;
	void __iomem *base;
	u32 row_size = FTBL_ROW_SIZE;
	u32 rates[MAX_L3_ENTRIES];
	unsigned long prev_freq = 0, freq;
	unsigned int count, i;
	u32 entry;
	int idx, ret;

	idx = of_property_match_string(dev->of_node, "reg-names", "l3tbl-base");
	if (idx < 0)
		return dev_err_probe(dev, idx, "missing L3 frequency table\n");

	ret = of_address_to_resource(dev->of_node, idx, &res);
	if (ret)
		return dev_err_probe(dev, ret, "invalid L3 frequency table resource\n");

	of_property_read_u32(dev->of_node, "qcom,ftbl-row-size", &row_size);
	if (row_size < sizeof(u32) || !IS_ALIGNED(row_size, sizeof(u32)) ||
	    resource_size(&res) < sizeof(u32))
		return dev_err_probe(dev, -EINVAL, "invalid L3 frequency table geometry\n");

	count = min_t(resource_size_t, MAX_L3_ENTRIES,
		      1 + (resource_size(&res) - sizeof(u32)) / row_size);
	base = ioremap(res.start, resource_size(&res));
	if (!base)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		entry = readl_relaxed(base + i * row_size);
		freq = entry & SRC_MASK ? XO_HZ * (entry & MULT_MASK) : INIT_HZ;
		if (!freq || (i && freq == prev_freq))
			break;

		rates[i] = freq / 1000;
		prev_freq = freq;
	}
	iounmap(base);

	if (!i)
		return dev_err_probe(dev, -ENODEV, "empty L3 frequency table\n");

	*freq_table = devm_kmemdup(dev, rates, i * sizeof(*rates), GFP_KERNEL);
	if (!*freq_table)
		return -ENOMEM;

	return i;
}

static int commit_epss_l3(struct dcvs_path *path, struct dcvs_freq *freqs,
						int cpu)
{
	struct dcvs_hw *hw = path->hw;
	struct epss_dev_data *d = path->data;
	u32 idx, offset;

	for (idx = 0; idx + 1 < hw->table_len; idx++)
		if (freqs->ib <= hw->freq_table[idx])
			break;

	offset = cpu < 0 ? d->l3_shared_offset : d->l3_percpu_offsets[cpu];
	writel_relaxed(idx, d->l3_base + offset);

	if (cpu < 0)
		path->cur_freq.ib = freqs->ib;

	return 0;
}

static int commit_epss_l3_shared(struct dcvs_path *path,
						struct dcvs_freq *freqs,
						u32 update_mask)
{
	return commit_epss_l3(path, freqs, -1);
}

static int commit_epss_l3_percpu(struct dcvs_path *path,
						struct dcvs_freq *freqs,
						u32 update_mask)
{
	return commit_epss_l3(path, freqs, smp_processor_id());
}

int commit_epss_l3_cpu(struct dcvs_path *path, struct dcvs_freq *freq, int cpu)
{
	return commit_epss_l3(path, freq, cpu);
}

static bool epss_valid_offset(struct epss_dev_data *data, u32 offset)
{
	return IS_ALIGNED(offset, sizeof(u32)) &&
	       offset <= data->l3_size - sizeof(u32);
}

#define PERCPU_OFFSETS	"qcom,percpu-offsets"
static int populate_percpu_offsets(struct device *dev, struct epss_dev_data *data)
{
	struct device_node *np = dev->of_node;
	struct device_node *table;
	unsigned int cpu;
	int words, ret;

	words = of_property_count_u32_elems(np, PERCPU_OFFSETS);
	/* A single cell can refer to a separate offset table. */
	table = words == 1 && nr_cpu_ids > 1 ?
		of_parse_phandle(np, PERCPU_OFFSETS, 0) : NULL;
	if (table)
		np = table;

	words = of_property_count_u32_elems(np, PERCPU_OFFSETS);
	if (words != nr_cpu_ids) {
		ret = -EINVAL;
		goto put_table;
	}

	data->l3_percpu_offsets = devm_kcalloc(dev, words, sizeof(u32), GFP_KERNEL);
	if (!data->l3_percpu_offsets) {
		ret = -ENOMEM;
		goto put_table;
	}

	ret = of_property_read_u32_array(np, PERCPU_OFFSETS,
					 data->l3_percpu_offsets, words);
	if (ret)
		goto put_table;

	for_each_possible_cpu(cpu) {
		if (!epss_valid_offset(data, data->l3_percpu_offsets[cpu])) {
			ret = -EINVAL;
			break;
		}
	}

put_table:
	of_node_put(table);
	return ret;
}

static int setup_epss_l3_device(struct device *dev, struct dcvs_path *path,
				bool shared)
{
	struct device_node *parent = dev->parent->of_node;
	struct epss_dev_data *data;
	struct resource res;
	int idx, ret;

	idx = of_property_match_string(parent, "reg-names", "l3-base");
	if (idx < 0)
		return dev_err_probe(dev, idx, "missing L3 vote registers\n");

	ret = of_address_to_resource(parent, idx, &res);
	if (ret)
		return dev_err_probe(dev, ret, "invalid L3 vote resource\n");

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->l3_size = resource_size(&res);
	if (data->l3_size < sizeof(u32))
		return -EINVAL;

	if (shared) {
		ret = of_property_read_u32(dev->of_node, "qcom,shared-offset",
					   &data->l3_shared_offset);
		if (!ret && !epss_valid_offset(data, data->l3_shared_offset))
			ret = -EINVAL;
	} else {
		ret = populate_percpu_offsets(dev, data);
	}
	if (ret)
		return dev_err_probe(dev, ret, "invalid L3 vote offsets\n");

	/* The CPU performance controller also maps this shared window. */
	data->l3_base = devm_ioremap(dev, res.start, data->l3_size);
	if (!data->l3_base)
		return -ENOMEM;

	path->data = data;
	path->commit_dcvs_freqs = shared ? commit_epss_l3_shared :
					   commit_epss_l3_percpu;
	return 0;
}

int setup_epss_l3_sp_device(struct device *dev, struct dcvs_hw *hw,
					struct dcvs_path *path)
{
	return setup_epss_l3_device(dev, path, true);
}

int setup_epss_l3_percpu_device(struct device *dev, struct dcvs_hw *hw,
					struct dcvs_path *path)
{
	return setup_epss_l3_device(dev, path, false);
}
