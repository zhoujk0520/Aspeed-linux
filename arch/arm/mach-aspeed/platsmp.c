/*
 * Copyright (C) ASPEED Technology Inc.
 * Shivah Shankar S <shivahshankar.shankarnarayanrao@aspeedtech.com>  
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/smp.h>
#include <asm/system_misc.h>

#define ASPEED_BOOT_ADDR_REG_OFFSET 0x00
#define ASPEED_BOOT_SIG_REG_OFFSET 0x04

void __iomem *secboot_base; 
unsigned char *wd_reset_base;

int aspeed_g6_system_restart(struct notifier_block *nb,
		unsigned long action, void *data)
{
	printk("WDT reset called\n");
	__raw_writel(0x10,wd_reset_base + 0x4);
	__raw_writel(0x4755,wd_reset_base + 0x8);
	__raw_writel(0x3,wd_reset_base + 0xC);
	return 0;
}

static struct notifier_block aspeed_g6_restart_nb = {
	.notifier_call	= aspeed_g6_system_restart,
	.priority	= 198,
};

static void aspeed_g6_early_reset_init(void)
{
	struct device_node *np;
	np = of_find_compatible_node(NULL, NULL, "aspeed,ast2600-reboot");
	if (!np) {
		pr_err("%s: no reboot node found\n", __func__);
		return;
	}
	wd_reset_base =(unsigned char *) of_iomap(np, 0);
	if (!wd_reset_base) {
		pr_err("%s: Unable to map I/O memory\n", __func__);
		return;
	}
	register_restart_handler(&aspeed_g6_restart_nb);
}

static void __init aspeed_g6_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *secboot_node;

	secboot_node = of_find_compatible_node(NULL, NULL, "aspeed,ast2600-smpmem");
	if (!secboot_node) {
		pr_err("secboot device node found!!\n");
		return;
	}

	secboot_base = of_iomap(secboot_node, 0);

	if (!secboot_base) {
		pr_err("could not map the secondary boot base!");
		return;
	}
	__raw_writel(0xBADABABA, secboot_base + ASPEED_BOOT_SIG_REG_OFFSET);

	aspeed_g6_early_reset_init();
}

static int aspeed_g6_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	__raw_writel(0, secboot_base + ASPEED_BOOT_ADDR_REG_OFFSET);
	__raw_writel(__pa_symbol(secondary_startup_arm), secboot_base + ASPEED_BOOT_ADDR_REG_OFFSET);
	__raw_writel((0xABBAAB00 | (cpu & 0xff)), secboot_base + ASPEED_BOOT_SIG_REG_OFFSET);
	wmb();
	/* barrier it to make sure everyone sees it */
	dsb_sev();

	return 0;
}

static void aspeed_g6_secondary_init(unsigned int cpu)
{
	/* restore cpuN go sign and addr */
	__raw_writel(0x0, secboot_base + ASPEED_BOOT_ADDR_REG_OFFSET);
	__raw_writel(0x0, secboot_base + ASPEED_BOOT_SIG_REG_OFFSET);
}

static const struct smp_operations aspeed_smp_ops __initconst = {
	.smp_prepare_cpus	= aspeed_g6_smp_prepare_cpus,
	.smp_boot_secondary	= aspeed_g6_boot_secondary,
	.smp_secondary_init	= aspeed_g6_secondary_init,
};

CPU_METHOD_OF_DECLARE(aspeed_smp, "aspeed,ast2600-smp", &aspeed_smp_ops);
