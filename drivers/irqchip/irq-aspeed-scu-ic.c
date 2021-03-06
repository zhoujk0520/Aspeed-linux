/*
 * irq-aspeed-scu.c - SCU IRQCHIP driver for the Aspeed SoC
 *
 * Copyright (C) ASPEED Technology Inc.
 * Ryan Chen <ryan_chen@aspeedtech.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/io.h>
#include <linux/module.h>

#define ASPEED_SCU_IRQ_NUM 	7

struct aspeed_scu_irq {
	void __iomem *regs;
	int irq;
	int parity_check;
	struct irq_domain *irq_domain;
};

static void aspeed_scu_irq_handler(struct irq_desc *desc)
{
	struct aspeed_scu_irq *scu_irq = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long bit, status, irq_sts;
	unsigned int bus_irq;

	chained_irq_enter(chip, desc);
	status = readl(scu_irq->regs);
	irq_sts = (status >> 16) & status;

	/* crash kernel if parity check fail on L1/L2 D$ and SRAM */
	if (scu_irq->parity_check)
	    BUG_ON((status & 0x1e000000));

	for_each_set_bit(bit, &irq_sts, ASPEED_SCU_IRQ_NUM) {
		bus_irq = irq_find_mapping(scu_irq->irq_domain, bit);
		generic_handle_irq(bus_irq);
		writel((status & 0x7f) | BIT(bit + 16), scu_irq->regs);
	}
	chained_irq_exit(chip, desc);
}

static void aspeed_scu_mask_irq(struct irq_data *data)
{
	struct aspeed_scu_irq *scu_irq = irq_data_get_irq_chip_data(data);
	unsigned int sbit = BIT(data->hwirq);

	writel(readl(scu_irq->regs) & ~sbit, scu_irq->regs);
}

static void aspeed_scu_unmask_irq(struct irq_data *data)
{
	struct aspeed_scu_irq *scu_irq = irq_data_get_irq_chip_data(data);
	unsigned int sbit = BIT(data->hwirq);
	
	writel((readl(scu_irq->regs) | sbit) & 0x7f, scu_irq->regs);
}

struct irq_chip aspeed_scu_irq_chip = {
	.name		= "scu-irq",
	.irq_mask	= aspeed_scu_mask_irq,
	.irq_unmask	= aspeed_scu_unmask_irq,
};

static int aspeed_scu_map_irq_domain(struct irq_domain *domain,
				  unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &aspeed_scu_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops aspeed_scu_irq_domain_ops = {
	.map = aspeed_scu_map_irq_domain,
};

static int __init aspeed_scu_intc_of_init(struct device_node *node,
					     struct device_node *parent)
{
	struct aspeed_scu_irq *scu_irq;
	int ret = 0;

	scu_irq = kzalloc(sizeof(*scu_irq), GFP_KERNEL);
	if (!scu_irq)
		return -ENOMEM;

	scu_irq->regs = of_iomap(node, 0);
	if (!scu_irq->regs) {
		ret = -ENOMEM;
		goto err_free;
	}

	scu_irq->irq = irq_of_parse_and_map(node, 0);
	if (scu_irq->irq < 0) {
		ret = scu_irq->irq;
		goto err_iounmap;
	}

	scu_irq->irq_domain = irq_domain_add_linear(
					node, ASPEED_SCU_IRQ_NUM,
					&aspeed_scu_irq_domain_ops, scu_irq);
	if (!scu_irq->irq_domain) {
		ret = -ENOMEM;
		goto err_iounmap;
	}

	irq_set_chained_handler_and_data(scu_irq->irq,
					 aspeed_scu_irq_handler, scu_irq);

	pr_info("scu-irq controller registered, irq %d\n", scu_irq->irq);
	
	scu_irq->parity_check = of_property_read_bool(node, "parity-check");
	if (scu_irq->parity_check)
		writel(readl(scu_irq->regs) | 0x1e00, scu_irq->regs);

	return 0;

err_iounmap:
	iounmap(scu_irq->regs);
err_free:
	kfree(scu_irq);
	return ret;
}

IRQCHIP_DECLARE(ast2400_scu_intc, "aspeed,ast2400-scu-ic", aspeed_scu_intc_of_init);
IRQCHIP_DECLARE(ast2500_scu_intc, "aspeed,ast2500-scu-ic", aspeed_scu_intc_of_init);
IRQCHIP_DECLARE(ast2600_scu_intc, "aspeed,ast2600-scu-ic", aspeed_scu_intc_of_init);
