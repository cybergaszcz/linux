// SPDX-License-Identifier: GPL-2.0
/*
 * SiFive L2 cache controller Driver
 *
 * Copyright (C) 2018-2019 SiFive, Inc.
 *
 */
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/device.h>
#include <asm/cacheinfo.h>
#include <soc/sifive/sifive_l2_cache.h>

#define SIFIVE_L2_DIRECCFIX_LOW 0x100
#define SIFIVE_L2_DIRECCFIX_HIGH 0x104
#define SIFIVE_L2_DIRECCFIX_COUNT 0x108

#define SIFIVE_L2_DIRECCFAIL_LOW 0x120
#define SIFIVE_L2_DIRECCFAIL_HIGH 0x124
#define SIFIVE_L2_DIRECCFAIL_COUNT 0x128

#define SIFIVE_L2_DATECCFIX_LOW 0x140
#define SIFIVE_L2_DATECCFIX_HIGH 0x144
#define SIFIVE_L2_DATECCFIX_COUNT 0x148

#define SIFIVE_L2_DATECCFAIL_LOW 0x160
#define SIFIVE_L2_DATECCFAIL_HIGH 0x164
#define SIFIVE_L2_DATECCFAIL_COUNT 0x168

#define SIFIVE_L2_FLUSH64 0x200

#define SIFIVE_L2_CONFIG 0x00
#define SIFIVE_L2_WAYENABLE 0x08
#define SIFIVE_L2_ECCINJECTERR 0x40

#ifdef CONFIG_SOC_STARFIVE_VIC7100
#define SIFIVE_L2_MAX_ECCINTR 4
#else
#define SIFIVE_L2_MAX_ECCINTR 3
#endif

#define SIFIVE_L2_FLUSH64_LINE_LEN 64

static void __iomem *l2_base = NULL;
#if !IS_ENABLED(CONFIG_SIFIVE_L2_IRQ_DISABLE)
static int g_irq[SIFIVE_L2_MAX_ECCINTR];
#endif
static struct riscv_cacheinfo_ops l2_cache_ops;

enum {
#ifdef CONFIG_SOC_STARFIVE_VIC7100
	DIR_UNCORR = 0,
	DIR_CORR,
#else
	DIR_CORR = 0,
#endif
	DATA_CORR,
	DATA_UNCORR,
};

#ifdef CONFIG_DEBUG_FS
static struct dentry *sifive_test;

static ssize_t l2_write(struct file *file, const char __user *data,
			size_t count, loff_t *ppos)
{
	unsigned int val;

	if (kstrtouint_from_user(data, count, 0, &val))
		return -EINVAL;
	if ((val < 0xFF) || (val >= 0x10000 && val < 0x100FF))
		writel(val, l2_base + SIFIVE_L2_ECCINJECTERR);
	else
		return -EINVAL;
	return count;
}

static const struct file_operations l2_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = l2_write
};

static void setup_sifive_debug(void)
{
	sifive_test = debugfs_create_dir("sifive_l2_cache", NULL);

	debugfs_create_file("sifive_debug_inject_error", 0200,
			    sifive_test, NULL, &l2_fops);
}
#endif

static void l2_config_read(void)
{
	u32 regval, val;

	regval = readl(l2_base + SIFIVE_L2_CONFIG);
	val = regval & 0xFF;
	pr_info("L2CACHE: No. of Banks in the cache: %d\n", val);
	val = (regval & 0xFF00) >> 8;
	pr_info("L2CACHE: No. of ways per bank: %d\n", val);
	val = (regval & 0xFF0000) >> 16;
	pr_info("L2CACHE: Sets per bank: %llu\n", (uint64_t)1 << val);
	val = (regval & 0xFF000000) >> 24;
	pr_info("L2CACHE: Bytes per cache block: %llu\n", (uint64_t)1 << val);

	regval = readl(l2_base + SIFIVE_L2_WAYENABLE);
	pr_info("L2CACHE: Index of the largest way enabled: %d\n", regval);
}

static const struct of_device_id sifive_l2_ids[] = {
	{ .compatible = "sifive,fu540-c000-ccache" },
	{ .compatible = "starfive,ccache0" },
	{ /* end of table */ },
};

static ATOMIC_NOTIFIER_HEAD(l2_err_chain);

int register_sifive_l2_error_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&l2_err_chain, nb);
}
EXPORT_SYMBOL_GPL(register_sifive_l2_error_notifier);

int unregister_sifive_l2_error_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(&l2_err_chain, nb);
}
EXPORT_SYMBOL_GPL(unregister_sifive_l2_error_notifier);

#ifdef CONFIG_SIFIVE_L2_FLUSH
void sifive_l2_flush64_range(unsigned long start, unsigned long len)
{
	unsigned long line;

	if(!l2_base) {
		pr_warn("L2CACHE: base addr invalid, skipping flush\n");
		return;
	}

	/* TODO: if (len == 0), skipping flush or going on? */
	if(!len) {
		pr_debug("L2CACHE: flush64 range @ 0x%lx(len:0)\n", start);
		return;
	}

	/* make sure the address is in the range */
	if(start < CONFIG_SIFIVE_L2_FLUSH_START ||
	   (start + len) > (CONFIG_SIFIVE_L2_FLUSH_START +
			     CONFIG_SIFIVE_L2_FLUSH_SIZE)) {
	/* disable warning message for better performance
		pr_warn("L2CACHE: flush64 out of range: %lx(%lx), skip flush\n",
			start, len);
	*/
		return;
	}

	mb();	/* sync */
	for (line = start; line < start + len;
	     line += SIFIVE_L2_FLUSH64_LINE_LEN) {
		writeq(line, l2_base + SIFIVE_L2_FLUSH64);
		mb();
	}
}
EXPORT_SYMBOL_GPL(sifive_l2_flush64_range);
#endif

static int l2_largest_wayenabled(void)
{
	return readl(l2_base + SIFIVE_L2_WAYENABLE) & 0xFF;
}

static ssize_t number_of_ways_enabled_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	return sprintf(buf, "%u\n", l2_largest_wayenabled());
}

static DEVICE_ATTR_RO(number_of_ways_enabled);

static struct attribute *priv_attrs[] = {
	&dev_attr_number_of_ways_enabled.attr,
	NULL,
};

static const struct attribute_group priv_attr_group = {
	.attrs = priv_attrs,
};

static const struct attribute_group *l2_get_priv_group(struct cacheinfo *this_leaf)
{
	/* We want to use private group for L2 cache only */
	if (this_leaf->level == 2)
		return &priv_attr_group;
	else
		return NULL;
}

#if !IS_ENABLED(CONFIG_SIFIVE_L2_IRQ_DISABLE)
static irqreturn_t l2_int_handler(int irq, void *device)
{
	unsigned int add_h, add_l;

#ifdef CONFIG_SOC_STARFIVE_VIC7100
	if (irq == g_irq[DIR_UNCORR]) {
		add_h = readl(l2_base + SIFIVE_L2_DIRECCFAIL_HIGH);
		add_l = readl(l2_base + SIFIVE_L2_DIRECCFAIL_LOW);
		pr_err("L2CACHE: DirFail @ 0x%08X.%08X\n", add_h, add_l);
		/* Reading this register clears the DirFail interrupt sig */
		readl(l2_base + SIFIVE_L2_DIRECCFAIL_COUNT);
		atomic_notifier_call_chain(&l2_err_chain, SIFIVE_L2_ERR_TYPE_UE,
					   "DirECCFail");
	}
#endif
	if (irq == g_irq[DIR_CORR]) {
		add_h = readl(l2_base + SIFIVE_L2_DIRECCFIX_HIGH);
		add_l = readl(l2_base + SIFIVE_L2_DIRECCFIX_LOW);
		pr_err("L2CACHE: DirError @ 0x%08X.%08X\n", add_h, add_l);
		/* Reading this register clears the DirError interrupt sig */
		readl(l2_base + SIFIVE_L2_DIRECCFIX_COUNT);
		atomic_notifier_call_chain(&l2_err_chain, SIFIVE_L2_ERR_TYPE_CE,
					   "DirECCFix");
	}
	if (irq == g_irq[DATA_CORR]) {
		add_h = readl(l2_base + SIFIVE_L2_DATECCFIX_HIGH);
		add_l = readl(l2_base + SIFIVE_L2_DATECCFIX_LOW);
		pr_err("L2CACHE: DataError @ 0x%08X.%08X\n", add_h, add_l);
		/* Reading this register clears the DataError interrupt sig */
		readl(l2_base + SIFIVE_L2_DATECCFIX_COUNT);
		atomic_notifier_call_chain(&l2_err_chain, SIFIVE_L2_ERR_TYPE_CE,
					   "DatECCFix");
	}
	if (irq == g_irq[DATA_UNCORR]) {
		add_h = readl(l2_base + SIFIVE_L2_DATECCFAIL_HIGH);
		add_l = readl(l2_base + SIFIVE_L2_DATECCFAIL_LOW);
		pr_err("L2CACHE: DataFail @ 0x%08X.%08X\n", add_h, add_l);
		/* Reading this register clears the DataFail interrupt sig */
		readl(l2_base + SIFIVE_L2_DATECCFAIL_COUNT);
		atomic_notifier_call_chain(&l2_err_chain, SIFIVE_L2_ERR_TYPE_UE,
					   "DatECCFail");
	}

	return IRQ_HANDLED;
}
#endif

static int __init sifive_l2_init(void)
{
	struct device_node *np;
	struct resource res;
#if !IS_ENABLED(CONFIG_SIFIVE_L2_IRQ_DISABLE)
	int i, rc;
#endif

	np = of_find_matching_node(NULL, sifive_l2_ids);
	if (!np)
		return -ENODEV;

	if (of_address_to_resource(np, 0, &res))
		return -ENODEV;

	l2_base = ioremap(res.start, resource_size(&res));
	if (!l2_base)
		return -ENOMEM;

#if !IS_ENABLED(CONFIG_SIFIVE_L2_IRQ_DISABLE)
	for (i = 0; i < SIFIVE_L2_MAX_ECCINTR; i++) {
		g_irq[i] = irq_of_parse_and_map(np, i);
		rc = request_irq(g_irq[i], l2_int_handler, 0, "l2_ecc", NULL);
		if (rc) {
			pr_err("L2CACHE: Could not request IRQ %d\n", g_irq[i]);
			return rc;
		}
	}
#endif

	l2_config_read();

	l2_cache_ops.get_priv_group = l2_get_priv_group;
	riscv_set_cacheinfo_ops(&l2_cache_ops);

#ifdef CONFIG_DEBUG_FS
	setup_sifive_debug();
#endif
	return 0;
}
device_initcall(sifive_l2_init);
