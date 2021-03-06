// SPDX-License-Identifier: GPL-2.0+

#define pr_fmt(fmt) "clk-aspeed: " fmt

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/ast2600-clock.h>
#include "clk-aspeed.h"

#define ASPEED_G6_NUM_CLKS		75

#define ASPEED_CLK2_OFFSET		32
#define ASPEED_G6_RESET_CTRL		0x40
#define ASPEED_G6_RESET_CTRL2		0x50

#define ASPEED_G6_CLK_SELECTION1	0x300
#define ASPEED_G6_CLK_SELECTION2	0x304
#define ASPEED_G6_CLK_SELECTION4	0x310
#define ASPEED_G6_CLK_SELECTION5	0x314

#define ASPEED_G6_MAC12_CLK_CTRL0	0x340
#define ASPEED_G6_MAC12_CLK_CTRL1	0x348
#define ASPEED_G6_MAC12_CLK_CTRL2	0x34C

#define ASPEED_G6_MAC34_CLK_CTRL0	0x350
#define ASPEED_G6_MAC34_CLK_CTRL1	0x358
#define ASPEED_G6_MAC34_CLK_CTRL2	0x35C

#define ASPEED_G6_MAC34_DRIVING_CTRL	0x458

#define ASPEED_G6_DEF_MAC12_DELAY_1G	0x0041b410
#define ASPEED_G6_DEF_MAC12_DELAY_100M	0x00417410
#define ASPEED_G6_DEF_MAC12_DELAY_10M	0x00417410
#define ASPEED_G6_DEF_MAC34_DELAY_1G	0x00104208
#define ASPEED_G6_DEF_MAC34_DELAY_100M	0x00104208
#define ASPEED_G6_DEF_MAC34_DELAY_10M	0x00104208

#define ASPEED_G6_CLK_STOP_CTRL		0x80
#define ASPEED_G6_CLK_STOP_CTRL2	0x90

#define ASPEED_G6_MISC_CTRL			0xC0
#define  UART_DIV13_EN		BIT(12)

#define ASPEED_APLL_PARAM	0x210
#define ASPEED_MPLL_PARAM	0x220
#define ASPEED_EPLL_PARAM	0x240
#define ASPEED_DPLL_PARAM	0x260

/* Globally visible clocks */
static DEFINE_SPINLOCK(aspeed_g6_clk_lock);

/* Keeps track of all clocks */
static struct clk_hw_onecell_data *aspeed_g6_clk_data;

static void __iomem *scu_g6_base;

//TODO list
//64 ~ xx : 0x300 ext emmc bit 15 -> --- map to 64 -->0x40
//64 ~ xx : 0x310 ext sd bit -> --- map to 65 -->0x41

static struct aspeed_gate_data aspeed_g6_gates[] = {
	/*  					     clk rst   			name		parent	flags */
	[ASPEED_CLK_GATE_MCLK] 			= {  0, -1, 			"mclk-gate",	"mpll",	CLK_IS_CRITICAL }, 	/* SDRAM */
	[ASPEED_CLK_GATE_ECLK] 			= {  1, -1, 			"eclk-gate",	"eclk",	0 }, 			/* Video Engine */
	[ASPEED_CLK_GATE_GCLK] 			= {  2,  ASPEED_RESET_2D,	"gclk-gate",	NULL,	0 }, 			/* 2D engine */
	//vclk parent - dclk/d1clk/hclk/mclk
	[ASPEED_CLK_GATE_VCLK] 			= {  3,  ASPEED_RESET_VIDEO,	"vclk-gate",	NULL,	0 }, 			/* Video Capture */
	[ASPEED_CLK_GATE_BCLK] 			= {  4,  ASPEED_RESET_PCI_VGA,	"bclk-gate",	"bclk",	CLK_IS_CRITICAL }, 	/* PCIe/PCI */
	//From dpll
	[ASPEED_CLK_GATE_DCLK] 			= {  5, -1, 			"dclk-gate",	NULL,	CLK_IS_CRITICAL }, 	/* DAC */
	[ASPEED_CLK_GATE_REF0CLK] 		= {  6, -1, 			"ref0clk-gate",	"clkin", CLK_IS_CRITICAL },
	[ASPEED_CLK_GATE_USBPORT2CLK] 		= {  7,  ASPEED_RESET_EHCI_P2, 	"usb-port2-gate",NULL,	0 }, 			/* USB2.0 Host port 2 */
	//reserved 8
	[ASPEED_CLK_GATE_USBUHCICLK] 		= {  9,  ASPEED_RESET_UHCI, 	"usb-uhci-gate", NULL,	0 }, 			/* USB1.1 (requires port 2 enabled) */
	//from dpll/epll/40mhz usb p1 phy/gpioc6/dp phy pll
	[ASPEED_CLK_GATE_D1CLK] 		= { 10,  ASPEED_RESET_CRT, 	"d1clk-gate",	"d1clk",0 }, 			/* GFX CRT */
	//reserved 11/12
	[ASPEED_CLK_GATE_YCLK] 			= { 13,  ASPEED_RESET_HACE, 	"yclk-gate",	NULL,	0 }, 			/* HAC */
	[ASPEED_CLK_GATE_USBPORT1CLK]		= { 14,  ASPEED_RESET_EHCI_P1, 	"usb-port1-gate",NULL,	0 }, 			/* USB2 hub/USB2 host port 1/USB1.1 dev */
	[ASPEED_CLK_GATE_UART5CLK] 		= { 15, -1, 			"uart5clk-gate", "uart",	0 }, 			/* UART5 */
	//reserved 16/19
	[ASPEED_CLK_GATE_MAC1CLK] 		= { 20,  ASPEED_RESET_MAC1, 	"mac1clk-gate",	"mac12",	0 }, 			/* MAC1 */
	[ASPEED_CLK_GATE_MAC2CLK] 		= { 21,  ASPEED_RESET_MAC2, 	"mac2clk-gate",	"mac12",	0 }, 			/* MAC2 */
	//reserved 22/23
	[ASPEED_CLK_GATE_RSACLK] 		= { 24,  ASPEED_RESET_HACE, 	"rsaclk-gate",	NULL,	0 }, 				/* HAC */
	[ASPEED_CLK_GATE_RVASCLK] 		= { 25,  ASPEED_RESET_RVAS, 	"rvasclk-gate",	NULL,	0 }, 				/* RVAS */
	//reserved 26
	[ASPEED_CLK_GATE_EMMCCLK]       = { 27, 16, "emmcclk-gate",     NULL,    0 },   /* For card clk */
	//SCU90
	[ASPEED_CLK_GATE_LCLK] 			= { 32,  ASPEED_RESET_LPC_ESPI, "lclk-gate",	NULL,	CLK_IS_CRITICAL }, 	/* LPC */
	[ASPEED_CLK_GATE_ESPICLK] 		= { 33, -1, 					"espiclk-gate",	NULL,	CLK_IS_CRITICAL }, 	/* eSPI */
	[ASPEED_CLK_GATE_REF1CLK] 		= { 34, -1, 					"ref1clk-gate",		"clkin", CLK_IS_CRITICAL },	
	//reserved 35
	[ASPEED_CLK_GATE_SDCLK] 		= { 36,  ASPEED_RESET_SD,	"sdclk-gate",	NULL,	0 },			/* SDIO/SD */
	[ASPEED_CLK_GATE_LHCCLK] 		= { 37, -1, 			"lhclk-gate",	"lhclk", 0 }, 			/* LPC master/LPC+ */
	//reserved 38 rsa no ues anymore
	[ASPEED_CLK_GATE_I3CDMACLK] 		= { 39,  ASPEED_RESET_I3C,	"i3cclk-gate",	NULL,	0 }, 			/* I3C_DMA */
	[ASPEED_CLK_GATE_I3C0CLK] 		= { 40,  ASPEED_RESET_I3C0, 	"i3c0clk-gate",	"i3cclk",	0 }, 		/* I3C0 */
	[ASPEED_CLK_GATE_I3C1CLK] 		= { 41,  ASPEED_RESET_I3C1, 	"i3c1clk-gate",	"i3cclk",	0 }, 		/* I3C1 */
	[ASPEED_CLK_GATE_I3C2CLK] 		= { 42,  ASPEED_RESET_I3C2, 	"i3c2clk-gate",	"i3cclk",	0 }, 				/* I3C2 */
	[ASPEED_CLK_GATE_I3C3CLK] 		= { 43,  ASPEED_RESET_I3C3, 	"i3c3clk-gate",	"i3cclk",	0 }, 				/* I3C3 */
	[ASPEED_CLK_GATE_I3C4CLK] 		= { 44,  ASPEED_RESET_I3C4, 	"i3c4clk-gate",	"i3cclk",	0 }, 				/* I3C4 */
	[ASPEED_CLK_GATE_I3C5CLK] 		= { 45,  ASPEED_RESET_I3C5, 	"i3c5clk-gate",	"i3cclk",	0 }, 				/* I3C5 */
	
	[ASPEED_CLK_GATE_I3C6CLK] 		= { 46,  ASPEED_RESET_I3C6, 	"i3c6clk-gate",	"i3cclk",	0 }, 				/* I3C6 */
	
	[ASPEED_CLK_GATE_UART1CLK] 		= { 48, -1, 					"uart1clk-gate",	"uxclk",		0 }, /* UART1 */
	[ASPEED_CLK_GATE_UART2CLK] 		= { 49, -1, 					"uart2clk-gate",	"uxclk",		0 }, /* UART2 */
	[ASPEED_CLK_GATE_UART3CLK] 		= { 50, -1, 					"uart3clk-gate",	"uxclk",		0 }, /* UART3 */
	[ASPEED_CLK_GATE_UART4CLK] 		= { 51, -1, 					"uart4clk-gate",	"uxclk",		0 }, /* UART4 */
	[ASPEED_CLK_GATE_MAC3CLK] 		= { 52,  ASPEED_RESET_MAC3, 	"mac3clk-gate",		"mac34",	0 }, 			/* MAC3 */
	[ASPEED_CLK_GATE_MAC4CLK] 		= { 53,  ASPEED_RESET_MAC4, 	"mac4clk-gate",		"mac34",	0 }, 			/* MAC4 */
	[ASPEED_CLK_GATE_UART6CLK] 		= { 54, -1, 					"uart6clk-gate",	"uxclk",	0 }, /* UART6 */
	[ASPEED_CLK_GATE_UART7CLK] 		= { 55, -1, 					"uart7clk-gate",	"uxclk",	0 }, /* UART7 */
	[ASPEED_CLK_GATE_UART8CLK] 		= { 56, -1, 					"uart8clk-gate",	"uxclk",	0 }, /* UART8 */
	[ASPEED_CLK_GATE_UART9CLK] 		= { 57, -1, 					"uart9clk-gate",	"uxclk",	0 }, /* UART9 */
	[ASPEED_CLK_GATE_UART10CLK] 	= { 58, -1, 					"uart10clk-gate",	"uxclk",	0 }, /* UART10 */
	[ASPEED_CLK_GATE_UART11CLK] 	= { 59, -1, 					"uart11clk-gate",	"uxclk",	0 }, /* UART11 */
	[ASPEED_CLK_GATE_UART12CLK] 	= { 60, -1, 					"uart12clk-gate",	"uxclk",	0 }, /* UART12 */
	[ASPEED_CLK_GATE_UART13CLK] 	= { 61, -1, 					"uart13clk-gate",	"uxclk",	0 }, /* UART13 */
	[ASPEED_CLK_GATE_FSICLK] 		= { 62, ASPEED_RESET_FSI, 		"fsiclk-gate",	NULL,	0 }, 		/* fsi */
};

static const char * const eclk_parent_names[] = {
	"mpll",
	"hpll",
	"dpll",
};

static const struct clk_div_table ast2600_eclk_div_table[] = {
	{ 0x0, 2 },
	{ 0x1, 2 },
	{ 0x2, 3 },
	{ 0x3, 4 },
	{ 0x4, 5 },
	{ 0x5, 6 },
	{ 0x6, 7 },
	{ 0x7, 8 },
	{ 0 }
};

static const struct clk_div_table ast2600_mac_div_table[] = {
	{ 0x0, 4 }, /* Yep, really. Aspeed confirmed this is correct */
	{ 0x1, 4 },
	{ 0x2, 6 },
	{ 0x3, 8 },
	{ 0x4, 10 },
	{ 0x5, 12 },
	{ 0x6, 14 },
	{ 0x7, 16 },
	{ 0 }
};

static const struct clk_div_table ast2600_div_table[] = {
	{ 0x0, 4 },
	{ 0x1, 8 },
	{ 0x2, 12 },
	{ 0x3, 16 },
	{ 0x4, 20 },
	{ 0x5, 24 },
	{ 0x6, 28 },
	{ 0x7, 32 },
	{ 0 }
};

static const struct clk_div_table ast2600_sd_div_table[] = {
	{ 0x0, 2 },
	{ 0x1, 4 },
	{ 0x2, 6 },
	{ 0x3, 8 },
	{ 0x4, 10 },
	{ 0x5, 12 },
	{ 0x6, 14 },
	{ 0x7, 16 },
	{ 0 }
};

static const struct clk_div_table ast2600_uart_div_table[] = {
	{ 0x0, 4 }, 
	{ 0x1, 2 },
	{ 0 }
};

//for dpll/epll/mpll
static struct clk_hw *aspeed_ast2600_calc_pll(const char *name, u32 val)
{
	unsigned int mult, div;
	if (val & BIT(24)) {
		/* Pass through mode */
		mult = div = 1;
	} else {
		/* F = 25Mhz * [(M + 2) / (n + 1)] / (p + 1) */
		u32 m = val  & 0x1fff;
		u32 n = (val >> 13) & 0x3f;
		u32 p = (val >> 19) & 0xf;
		mult = (m + 1) / (n + 1);
		div = (p + 1);
	}
	return clk_hw_register_fixed_factor(NULL, name, "clkin", 0,
			mult, div);
};

static struct clk_hw *aspeed_ast2600_calc_hpll(const char *name, u32 hwstrap, u32 val)
{
	unsigned int mult, div;
	/* 
	HPLL Numerator (M) = fix 0x5F when SCU500[10]=1
						 fix 0xBF when SCU500[10]=0 and SCU500[8]=1
	SCU200[12:0] (default 0x8F) when SCU510[10]=0 and SCU510[8]=0 
	HPLL Denumerator (N) =	SCU200[18:13] (default 0x2)
	HPLL Divider (P)	 =	SCU200[22:19] (default 0x0)
	HPLL Bandwidth Adj (NB) =  fix 0x2F when SCU500[10]=1
							   fix 0x5F when SCU500[10]=0 and SCU500[8]=1
	SCU204[11:0] (default 0x31) when SCU500[10]=0 and SCU500[8]=0 
	*/
	if (val & BIT(24)) {
		/* Pass through mode */
		mult = div = 1;
	} else {
		/* F = 25Mhz * [(M + 2) / (n + 1)] / (p + 1) */
		u32 m = val  & 0x1fff;
		u32 n = (val >> 13) & 0x3f;
		u32 p = (val >> 19) & 0xf;
		if(hwstrap & BIT(10))
			m = 0x5F;
		else {
			if(hwstrap & BIT(8))
				m = 0xBF;
			//otherwise keep default 0x8F
		}
		mult = (m + 1) / (n + 1);
		div = (p + 1);
	}
	return clk_hw_register_fixed_factor(NULL, name, "clkin", 0,
			mult, div);
};

//for apll
static struct clk_hw *aspeed_ast2600_calc_apll(const char *name, u32 val)
{
	unsigned int mult, div;

	if (val & BIT(20)) {
		/* Pass through mode */
		mult = div = 1;
	} else {
		/* F = 25Mhz * (2-od) * [(m + 2) / (n + 1)] */
		u32 m = (val >> 5) & 0x3f;
		u32 od = (val >> 4) & 0x1;
		u32 n = val & 0xf;

		mult = (2 - od) * (m + 2);
		div = n + 1;
	}
	return clk_hw_register_fixed_factor(NULL, name, "clkin", 0,
			mult, div);

};

struct aspeed_g6_clk_soc_data {
	const struct clk_div_table *div_table;
	const struct clk_div_table *eclk_div_table;
	const struct clk_div_table *mac_div_table;
	struct clk_hw *(*calc_pll)(const char *name, u32 val);
	unsigned int nr_resets;
};

static const struct aspeed_clk_soc_data ast2600_data = {
	.div_table = ast2600_div_table,
	.mac_div_table = ast2600_mac_div_table,
	.eclk_div_table = ast2600_eclk_div_table,	
};

static int aspeed_g6_clk_is_enabled(struct clk_hw *hw)
{
	u32 clk = 0;
	u32 rst = 0;
	u32 reg;	
	u32 enval = 0;
	int clk_sel_flag = 0;
	struct aspeed_clk_gate *gate = to_aspeed_clk_gate(hw);

	if(gate->reset_idx & 0x20)
		rst = BIT(gate->reset_idx - 32);
	else
		rst = BIT((gate->reset_idx));

	if(gate->clock_idx & 0x40)
		clk_sel_flag = 1;
	else if(gate->clock_idx & 0x20)
		clk = BIT(gate->clock_idx - 32);
	else
		clk = BIT((gate->clock_idx));

	if(clk_sel_flag) {
		if (gate->clock_idx == 64) {
			//ext emmc 
			regmap_read(gate->map, ASPEED_G6_CLK_SELECTION1, &reg);
			printk("ext emmc 0x300 %x \n", reg);
			return (reg & BIT(15)) ? 1 : 0;
		} else if (gate->clock_idx == 65) {
			//ext sd
			regmap_read(gate->map, ASPEED_G6_CLK_SELECTION4, &reg);
			printk("ext sd 0x310 %x \n", reg);
			return (reg & BIT(31)) ? 1 : 0;
		} else {
			printk("error \n");
			return 0;
		}
	} else {
		enval = (gate->flags & CLK_GATE_SET_TO_DISABLE) ? 0 : clk;
		/*
		 * If the IP is in reset, treat the clock as not enabled,
		 * this happens with some clocks such as the USB one when
		 * coming from cold reset. Without this, aspeed_clk_enable()
		 * will fail to lift the reset.
		 */
		if (gate->reset_idx >= 0) {
			if(gate->reset_idx & 0x20)
				regmap_read(gate->map, ASPEED_G6_RESET_CTRL2, &reg);
			else
				regmap_read(gate->map, ASPEED_G6_RESET_CTRL, &reg);

			if (reg & rst)
				return 0;
		}

		if(gate->clock_idx & 0x20)
			regmap_read(gate->map, ASPEED_G6_CLK_STOP_CTRL2, &reg);
		else
			regmap_read(gate->map, ASPEED_G6_CLK_STOP_CTRL, &reg);

		return ((reg & clk) == enval) ? 1 : 0;
	}
}

static int aspeed_g6_clk_enable(struct clk_hw *hw)
{
	struct aspeed_clk_gate *gate = to_aspeed_clk_gate(hw);
	unsigned long flags;
	u32 enval;
	u32 clk = 0;
	u32 rst = 0;
	int clk_sel_flag = 0;

	if(gate->reset_idx & 0x20)
		rst = BIT(gate->reset_idx - 32);
	else
		rst = BIT((gate->reset_idx));
	if(gate->clock_idx & 0x40)
		clk_sel_flag = 1;
	else if(gate->clock_idx & 0x20)
		clk = BIT(gate->clock_idx - 32);
	else
		clk = BIT((gate->clock_idx));
		

	spin_lock_irqsave(gate->lock, flags);
	if (aspeed_g6_clk_is_enabled(hw)) {
		spin_unlock_irqrestore(gate->lock, flags);
		return 0;
	}

	if (clk_sel_flag) {
		printk("todo ext emmc /sd clk \n");
	} else {
		if (gate->reset_idx >= 0) {
			/* Put IP in reset */
			if(gate->reset_idx & 0x20)
				regmap_write(gate->map, ASPEED_G6_RESET_CTRL2, rst);
			else
				regmap_write(gate->map, ASPEED_G6_RESET_CTRL, rst);
			/* Delay 100us */
			udelay(100);
		}

		/* Enable clock */
#if 0		
		if(gate->clock_idx == aspeed_g6_gates[ASPEED_CLK_GATE_SDEXTCLK].clock_idx) {
			/* sd ext clk */
			printk("enable sd card clk xxxxx\n");
			regmap_update_bits(gate->map, ASPEED_G6_CLK_SELECTION4, BIT(31), BIT(31));
		} else if(gate->clock_idx == aspeed_g6_gates[ASPEED_CLK_GATE_EMMCEXTCLK].clock_idx) {
			/* emmc ext clk */
			regmap_update_bits(gate->map, ASPEED_G6_CLK_SELECTION1, BIT(15), BIT(15));
			printk("enable emmc card clk xxxxx\n");
		} else {
#endif			
			enval = (gate->flags & CLK_GATE_SET_TO_DISABLE) ? 0 : clk;
			if(enval) {
				if(gate->clock_idx & 0x20)
					regmap_write(gate->map, ASPEED_G6_CLK_STOP_CTRL2, clk);
				else
					regmap_write(gate->map, ASPEED_G6_CLK_STOP_CTRL, clk);
			} else {
				if(gate->clock_idx & 0x20)
					regmap_write(gate->map, ASPEED_G6_CLK_STOP_CTRL2 + 0x04, clk);
				else
					regmap_write(gate->map, ASPEED_G6_CLK_STOP_CTRL + 0x04, clk);
			}
//		}

		if (gate->reset_idx >= 0) {
			/* A delay of 10ms is specified by the ASPEED docs */
			mdelay(10);
			/* Take IP out of reset */
			if(gate->reset_idx & 0x20)
				regmap_write(gate->map, ASPEED_G6_RESET_CTRL2 + 0x04, rst);
			else
				regmap_write(gate->map, ASPEED_G6_RESET_CTRL + 0x04, rst);
		}
	}

	spin_unlock_irqrestore(gate->lock, flags);

	return 0;
}

static void aspeed_g6_clk_disable(struct clk_hw *hw)
{
	struct aspeed_clk_gate *gate = to_aspeed_clk_gate(hw);
	unsigned long flags;
	u32 clk;
	u32 enval;
	int clk_sel_flag = 0;

	if(gate->clock_idx & 0x40) {
		clk_sel_flag = 1;
	} else if(gate->clock_idx & 0x1f)
		clk = BIT((gate->clock_idx));
	else
		clk = BIT(gate->clock_idx - 32);

	spin_lock_irqsave(gate->lock, flags);

	if(clk_sel_flag) {
		if(gate->clock_idx == 64) {
			regmap_update_bits(gate->map, ASPEED_G6_CLK_SELECTION1, BIT(15), 0);
		} else if(gate->clock_idx == 65) {
			regmap_update_bits(gate->map, ASPEED_G6_CLK_SELECTION4, BIT(31), 0);
		} else 	
			printk("todo disable clk \n");
	} else {
		enval = (gate->flags & CLK_GATE_SET_TO_DISABLE) ? clk : 0;

		if(enval) {
			if(gate->clock_idx & 0x20)
				regmap_write(gate->map, ASPEED_G6_CLK_STOP_CTRL2, clk);
			else
				regmap_write(gate->map, ASPEED_G6_CLK_STOP_CTRL, clk);
		} else {
			if(gate->clock_idx & 0x20)
				regmap_write(gate->map, ASPEED_G6_CLK_STOP_CTRL2 + 0x04, clk);
			else
				regmap_write(gate->map, ASPEED_G6_CLK_STOP_CTRL + 0x04, clk);
		}
	}
	spin_unlock_irqrestore(gate->lock, flags);
}

static const struct clk_ops aspeed_g6_clk_gate_ops = {
	.enable = aspeed_g6_clk_enable,
	.disable = aspeed_g6_clk_disable,
	.is_enabled = aspeed_g6_clk_is_enabled,
};

static int aspeed_g6_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	struct aspeed_reset *ar = to_aspeed_reset(rcdev);

	if(id >= 32) 
		return regmap_write(ar->map, ASPEED_G6_RESET_CTRL2 + 0x04, BIT(id - 32));
	else
		return regmap_write(ar->map, ASPEED_G6_RESET_CTRL + 0x04, BIT(id));
}

static int aspeed_g6_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct aspeed_reset *ar = to_aspeed_reset(rcdev);

	if(id >= 32) 
		return regmap_write(ar->map, ASPEED_G6_RESET_CTRL2, BIT(id - 32));
	else
		return regmap_write(ar->map, ASPEED_G6_RESET_CTRL, BIT(id));
}

static int aspeed_g6_reset_status(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct aspeed_reset *ar = to_aspeed_reset(rcdev);
	u32 reg = ASPEED_G6_RESET_CTRL;
	int ret, val;

	if (id >= 32) {
		id -= 32;
		reg = ASPEED_G6_RESET_CTRL2;
	}

	ret = regmap_read(ar->map, reg, &val);
	if (ret)
		return ret;

	return !!(val & BIT(id));
}

static const struct reset_control_ops aspeed_g6_reset_ops = {
	.assert = aspeed_g6_reset_assert,
	.deassert = aspeed_g6_reset_deassert,
	.status = aspeed_g6_reset_status,
};

static struct clk_hw *aspeed_g6_clk_hw_register_gate(struct device *dev,
		const char *name, const char *parent_name, unsigned long flags,
		struct regmap *map, u8 clock_idx, u8 reset_idx,
		u8 clk_gate_flags, spinlock_t *lock)
{
	struct aspeed_clk_gate *gate;
	struct clk_init_data init;
	struct clk_hw *hw;
	int ret;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &aspeed_g6_clk_gate_ops;
	init.flags = flags;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.num_parents = parent_name ? 1 : 0;

	gate->map = map;
	gate->clock_idx = clock_idx;
	gate->reset_idx = reset_idx;
	gate->flags = clk_gate_flags;
	gate->lock = lock;
	gate->hw.init = &init;

	hw = &gate->hw;
	ret = clk_hw_register(dev, hw);
	if (ret) {
		kfree(gate);
		hw = ERR_PTR(ret);
	}

	return hw;
}

static const char * const vclk_parent_names[] = {
	"dpll",
	"d1pll",
	"hclk",
	"mclk",
};

static const char * const d1clk_parent_names[] = {
	"dpll",
	"epll",
	"usb-phy-40m",
	"gpioc6_clkin",
	"dp_phy_pll",
};

static int aspeed_g6_clk_probe(struct platform_device *pdev)
{
	const struct aspeed_clk_soc_data *soc_data;
	struct device *dev = &pdev->dev;
	struct aspeed_reset *ar;
	struct regmap *map;
	struct clk_hw *hw;
	u32 val, div, mult;
	int i, ret;

	map = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(map)) {
		dev_err(dev, "no syscon regmap\n");
		return PTR_ERR(map);
	}

	ar = devm_kzalloc(dev, sizeof(*ar), GFP_KERNEL);
	if (!ar)
		return -ENOMEM;

	ar->map = map;

	ar->rcdev.owner = THIS_MODULE;
	ar->rcdev.nr_resets = 64;
	ar->rcdev.ops = &aspeed_g6_reset_ops;
	ar->rcdev.of_node = dev->of_node;

	ret = devm_reset_controller_register(dev, &ar->rcdev);
	if (ret) {
		dev_err(dev, "could not register reset controller\n");
		return ret;
	}

	/* SoC generations share common layouts but have different divisors */
	soc_data = of_device_get_match_data(dev);
	if (!soc_data) {
		dev_err(dev, "no match data for platform\n");
		return -EINVAL;
	}

	//uxclk
	regmap_read(map, 0x338, &val);
	div = ((val >> 8) & 0x3ff) * 2;
	mult = val & 0xff;

	hw = clk_hw_register_fixed_factor(dev, "uxclk", "uartx", 0,
			mult, div);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_UXCLK] = hw;

	//huxclk
	regmap_read(map, 0x33c, &val);
	div = ((val >> 8) & 0x3ff) * 2;
	mult = val & 0xff;

	hw = clk_hw_register_fixed_factor(dev, "huxclk", "uartx", 0,
			mult, div);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_HUXCLK] = hw;

	regmap_read(map, 0x04, &val);
	if((val & GENMASK(23, 16)) >> 16) {
		//A1 use mpll for fit 200Mhz
		regmap_update_bits(map, ASPEED_G6_CLK_SELECTION1, GENMASK(14, 11), BIT(11));

		/* EMMC ext clock divider */
		hw = clk_hw_register_gate(dev, "emmc_extclk_gate", "mpll", 0,
						scu_g6_base + ASPEED_G6_CLK_SELECTION1, 15, 0,
						&aspeed_g6_clk_lock);
		if (IS_ERR(hw))
				return PTR_ERR(hw);

		//ast2600 emmc clk should under 200Mhz
		hw = clk_hw_register_divider_table(dev, "emmc_extclk", "emmc_extclk_gate", 0,
						scu_g6_base + ASPEED_G6_CLK_SELECTION1, 12, 3, 0,
						ast2600_sd_div_table,
						&aspeed_g6_clk_lock);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		aspeed_g6_clk_data->hws[ASPEED_CLK_EMMC] = hw;
	} else {
		/* EMMC ext clock divider */
		hw = clk_hw_register_gate(dev, "emmc_extclk_gate", "hpll", 0,
						scu_g6_base + ASPEED_G6_CLK_SELECTION1, 15, 0,
						&aspeed_g6_clk_lock);
		if (IS_ERR(hw))
				return PTR_ERR(hw);
		
		//ast2600 emmc clk should under 200Mhz
		hw = clk_hw_register_divider_table(dev, "emmc_extclk", "emmc_extclk_gate", 0,
						scu_g6_base + ASPEED_G6_CLK_SELECTION1, 12, 3, 0,
						ast2600_div_table,
						&aspeed_g6_clk_lock);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		aspeed_g6_clk_data->hws[ASPEED_CLK_EMMC] = hw;
	}

	regmap_read(map, 0x310, &val);
	if(val & BIT(8)) {
		/* SD/SDIO clock divider and gate */
		hw = clk_hw_register_gate(dev, "sd_extclk_gate", "apll", 0,
						scu_g6_base + ASPEED_G6_CLK_SELECTION4, 31, 0,
						&aspeed_g6_clk_lock);
		if (IS_ERR(hw))
				return PTR_ERR(hw);
	} else {
		/* SD/SDIO clock divider and gate */
		hw = clk_hw_register_gate(dev, "sd_extclk_gate", "hclk", 0,
						scu_g6_base + ASPEED_G6_CLK_SELECTION4, 31, 0,
						&aspeed_g6_clk_lock);
		if (IS_ERR(hw))
				return PTR_ERR(hw);
	}
	
	hw = clk_hw_register_divider_table(dev, "sd_extclk", "sd_extclk_gate",
					0, scu_g6_base + ASPEED_G6_CLK_SELECTION4, 28, 3, 0,
					ast2600_sd_div_table,
					&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
			return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_SDIO] = hw;

	/* MAC1/2 RMII 50MHz RCLK */
	hw = clk_hw_register_fixed_rate(dev, "mac12rclk", "hpll", 0, 50000000);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	//mac12 clk - check
	/* MAC AHB bus clock divider */
	hw = clk_hw_register_divider_table(dev, "mac12", "hpll", 0,
			scu_g6_base + 0x300, 16, 3, 0,
			soc_data->mac_div_table,
			&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_MAC12] = hw;

	/* RMII1 50MHz (RCLK) output enable */
	hw = clk_hw_register_gate(dev, "mac1rclk", "mac12rclk", 0,
				  scu_g6_base + ASPEED_G6_MAC12_CLK_CTRL0, 29, 0,
				  &aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_MAC1RCLK] = hw;

	/* RMII2 50MHz (RCLK) output enable */
	hw = clk_hw_register_gate(dev, "mac2rclk", "mac12rclk", 0,
				  scu_g6_base + ASPEED_G6_MAC12_CLK_CTRL0, 30, 0,
				  &aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_MAC2RCLK] = hw;

	/* MAC1/2 RMII 50MHz RCLK */
	hw = clk_hw_register_fixed_rate(dev, "mac34rclk", "hclk", 0, 50000000);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	//mac34 clk - check
	/* MAC AHB bus clock divider */
	hw = clk_hw_register_divider_table(dev, "mac34", "hpll", 0,
			scu_g6_base + 0x310, 24, 3, 0,
			soc_data->mac_div_table,
			&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_MAC34] = hw;

	/* RMII3 50MHz (RCLK) output enable */
	hw = clk_hw_register_gate(dev, "mac3rclk", "mac34rclk", 0,
			scu_g6_base + ASPEED_G6_MAC34_CLK_CTRL0, 29, 0,
			&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_MAC3RCLK] = hw;

	/* RMII4 50MHz (RCLK) output enable */
	hw = clk_hw_register_gate(dev, "mac4rclk", "mac34rclk", 0,
			scu_g6_base + ASPEED_G6_MAC34_CLK_CTRL0, 30, 0,
			&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_MAC4RCLK] = hw;

	/* LPC Host (LHCLK) clock divider */
	hw = clk_hw_register_divider_table(dev, "lhclk", "hpll", 0,
			scu_g6_base + ASPEED_G6_CLK_SELECTION1, 20, 3, 0,
			soc_data->div_table,
			&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_LHCLK] = hw;

	//gfx d1clk : use dp clk
#if 1	
	regmap_update_bits(map, ASPEED_G6_CLK_SELECTION1, GENMASK(10, 8), BIT(10));
	/* SoC Display clock selection */
	hw = clk_hw_register_mux(dev, "d1clk", d1clk_parent_names,
			ARRAY_SIZE(d1clk_parent_names), 0,
			scu_g6_base + 0x300, 8, 3, 0,
			&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_D1CLK] = hw;
	//d1 clk div 0x308[17:15] x [14:12] - 8,7,6,5,4,3,2,1
	regmap_write(map, 0x308, 0xa000); //2x3 = 6
	//d1 clk div 0x308[17:15] x [14:12] - 8,7,6,5,4,3,2,1
	regmap_write(map, 0x308, 0xa000); //2x3 = 6
#else
	regmap_update_bits(map, ASPEED_G6_CLK_SELECTION1, GENMASK(10, 8), BIT(9));
	/* SoC Display clock selection */
	hw = clk_hw_register_mux(dev, "d1clk", d1clk_parent_names,
			ARRAY_SIZE(d1clk_parent_names), 0,
			scu_g6_base + 0x300, 8, 3, 0,
			&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_D1CLK] = hw;
#endif

	//bclk -check
	/* P-Bus (BCLK) clock divider */
	hw = clk_hw_register_divider_table(dev, "bclk", "hpll", 0,
			scu_g6_base + 0x300, 20, 3, 0,
			soc_data->div_table,
			&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_BCLK] = hw;

	//vclk - check 
	/* Video Capture clock selection */
	hw = clk_hw_register_mux(dev, "vclk", vclk_parent_names,
			ARRAY_SIZE(vclk_parent_names), 0,
			scu_g6_base + 0x304, 12, 3, 0,
			&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_VCLK] = hw;

	//eclk -check
	regmap_update_bits(map, ASPEED_G6_CLK_SELECTION1, GENMASK(31, 28), 0);
	/* Video Engine clock divider */ 
	hw = clk_hw_register_divider_table(dev, "eclk", NULL, 0,
			scu_g6_base + 0x300, 28, 3, 0,
			soc_data->eclk_div_table,
			&aspeed_g6_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_g6_clk_data->hws[ASPEED_CLK_ECLK] = hw;

	//fix for uartx parent 
	for(i = 0; i < 13; i++) {
		if((i < 6) & (i != 4)) {
			regmap_read(map, 0x310, &val);
			if(val & BIT(i))
				aspeed_g6_gates[ASPEED_CLK_GATE_UART1CLK + i].parent_name = "huxclk";
			else
				aspeed_g6_gates[ASPEED_CLK_GATE_UART1CLK + i].parent_name = "uxclk";
		}
		if(i == 4)
			aspeed_g6_gates[ASPEED_CLK_GATE_UART1CLK + i].parent_name = "uart";
		if((i > 5) & (i != 4)) {
			regmap_read(map, 0x314, &val);
			if(val & BIT(i))
				aspeed_g6_gates[ASPEED_CLK_GATE_UART1CLK + i].parent_name = "huxclk";
			else
				aspeed_g6_gates[ASPEED_CLK_GATE_UART1CLK + i].parent_name = "uxclk";
		}
	}

	for (i = 0; i < ARRAY_SIZE(aspeed_g6_gates); i++) {
		const struct aspeed_gate_data *gd = &aspeed_g6_gates[i];
		u32 gate_flags;

		/* Special case: the USB port 1 clock (bit 14) is always
		 * working the opposite way from the other ones.
		 */
		gate_flags = (gd->clock_idx == 14) ? 0 : CLK_GATE_SET_TO_DISABLE;
		hw = aspeed_g6_clk_hw_register_gate(dev,
				gd->name,
				gd->parent_name,
				gd->flags,
				map,
				gd->clock_idx,
				gd->reset_idx,
				gate_flags,
				&aspeed_g6_clk_lock);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		aspeed_g6_clk_data->hws[i] = hw;
	}

	return 0;
};

static const struct of_device_id aspeed_g6_clk_dt_ids[] = {
	{ .compatible = "aspeed,ast2600-scu", .data = &ast2600_data },
	{ }
};

static struct platform_driver aspeed_g6_clk_driver = {
	.probe  = aspeed_g6_clk_probe,
	.driver = {
		.name = "aspeed-g6-clk",
		.of_match_table = aspeed_g6_clk_dt_ids,
		.suppress_bind_attrs = true,
	},
};

static int __init aspeed_g6_clk_init(void)
{
	return platform_driver_register(&aspeed_g6_clk_driver);
}
core_initcall(aspeed_g6_clk_init);

#define ASPEED_HPLL_PARAM	0x200

static u32 ast2600_a0_axi_ahb_div_table[] = {
	2, 2, 3, 4,
};

static u32 ast2600_a1_axi_ahb_div0_table[] = {
	3, 2, 3, 4,
};

static u32 ast2600_a1_axi_ahb_div1_table[] = {
	3, 4, 6, 8,
};

static u32 ast2600_a1_axi_ahb_default_table[] = {
	3, 4, 3, 4, 2, 2, 2, 2,
};

static void __init aspeed_ast2600_cc(struct regmap *map)
{
	struct clk_hw *hw;
	u32 val, freq, div, chip_id, axi_div, ahb_div, hwstrap;

	freq = 25000000;

	hw = clk_hw_register_fixed_rate(NULL, "clkin", NULL, 0, freq);
	pr_debug("clkin @%u MHz\n", freq / 1000000);

	/*
	 * High-speed PLL clock derived from the crystal. This the CPU clock,
	 * and we assume that it is enabled
	 */
	regmap_read(map, ASPEED_HPLL_PARAM, &val);
	regmap_read(map, 0x500, &hwstrap);	
	aspeed_g6_clk_data->hws[ASPEED_CLK_HPLL] = aspeed_ast2600_calc_hpll("hpll", hwstrap, val);

	regmap_read(map, ASPEED_MPLL_PARAM, &val);
	aspeed_g6_clk_data->hws[ASPEED_CLK_MPLL] = aspeed_ast2600_calc_pll("mpll", val);

	regmap_read(map, ASPEED_DPLL_PARAM, &val);
	aspeed_g6_clk_data->hws[ASPEED_CLK_DPLL] = aspeed_ast2600_calc_pll("dpll", val);

	regmap_read(map, ASPEED_EPLL_PARAM, &val);
	aspeed_g6_clk_data->hws[ASPEED_CLK_EPLL] = aspeed_ast2600_calc_pll("epll", val);

	regmap_read(map, ASPEED_APLL_PARAM, &val);
	aspeed_g6_clk_data->hws[ASPEED_CLK_APLL] = aspeed_ast2600_calc_apll("apll", val);

	//uart5 
	regmap_read(map, ASPEED_G6_MISC_CTRL, &val);
	if (val & UART_DIV13_EN)
		div = 0x2;
	else
		div = 0;
	regmap_read(map, ASPEED_G6_CLK_SELECTION2, &val);
	if (val & BIT(14))
		div |= 0x1;

	switch(div) {
		case 0:
			freq = 24000000;
			break;
		case 1:
			freq = 192000000;
			break;
		case 2:
			freq = 24000000/13;
			break;
		case 3:
			freq = 192000000/13;
			break;
	}

	aspeed_g6_clk_data->hws[ASPEED_CLK_UART] = clk_hw_register_fixed_rate(NULL, "uart", NULL, 0, freq);

	/* UART1~13 clock div13 setting except uart5 */
	regmap_read(map, ASPEED_G6_CLK_SELECTION5, &val);

	switch (val & 0x3) {
		case 0: //apll div 4
			aspeed_g6_clk_data->hws[ASPEED_CLK_UARTX] = clk_hw_register_fixed_factor(NULL, "uartx", "apll", 0, 1, 4);
			break;
		case 1:	//apll div 2
			aspeed_g6_clk_data->hws[ASPEED_CLK_UARTX] = clk_hw_register_fixed_factor(NULL, "uartx", "apll", 0, 1, 2);
			break;
		case 2:
			aspeed_g6_clk_data->hws[ASPEED_CLK_UARTX] = clk_hw_register_fixed_factor(NULL, "uartx", "apll", 0, 1, 1);
			break;
		case 3:
			aspeed_g6_clk_data->hws[ASPEED_CLK_UARTX] = clk_hw_register_fixed_factor(NULL, "uartx", "ahb", 0, 1, 1);
			break;
	}

	regmap_read(map, 0x04, &chip_id);

	if (chip_id & BIT(16)) {
		//ast2600a1
		if (hwstrap & BIT(16)) {
			ast2600_a1_axi_ahb_div1_table[0] = ast2600_a1_axi_ahb_default_table[(hwstrap >> 8) & 0x3];
			axi_div = 1;
			ahb_div = ast2600_a1_axi_ahb_div1_table[(hwstrap >> 11) & 0x3];
		} else {
			ast2600_a1_axi_ahb_div0_table[0] = ast2600_a1_axi_ahb_default_table[(hwstrap >> 8) & 0x3];
			axi_div = 2;
			ahb_div = ast2600_a1_axi_ahb_div0_table[(hwstrap >> 11) & 0x3];
		}
	} else {
		//ast2600a0 : fix axi = hpll/2
		axi_div = 2;
		ahb_div = ast2600_a0_axi_ahb_div_table[(hwstrap >> 11) & 0x3];
	}

	hw = clk_hw_register_fixed_factor(NULL, "ahb", "hpll", 0, 1, axi_div * ahb_div);
	aspeed_g6_clk_data->hws[ASPEED_CLK_AHB] = hw;

	regmap_read(map, ASPEED_G6_CLK_SELECTION1, &val);
	val = (val >> 23) & 0x7;
	div = 4 * (val + 1);
	hw = clk_hw_register_fixed_factor(NULL, "apb1", "hpll", 0, 1, div);
	aspeed_g6_clk_data->hws[ASPEED_CLK_APB1] = hw;	

	regmap_read(map, ASPEED_G6_CLK_SELECTION4, &val);
	val = (val >> 9) & 0x7;
	div = 2 * (val + 1);
	hw = clk_hw_register_fixed_factor(NULL, "apb2", "ahb", 0, 1, div);
	aspeed_g6_clk_data->hws[ASPEED_CLK_APB2] = hw;	

	/* USB 2.0 port1 phy 40MHz clock */
	hw = clk_hw_register_fixed_rate(NULL, "usb-phy-40m", NULL, 0, 40000000);
	aspeed_g6_clk_data->hws[ASPEED_CLK_USBPHY_40M] = hw;

	//* i3c clock */
	regmap_read(map, ASPEED_G6_CLK_SELECTION5, &val);
	if(val & BIT(31)) {
		val = (val >> 28) & 0x7;
		if(val)
			div = val + 1;
		else
			div = val + 2;
		hw = clk_hw_register_fixed_factor(NULL, "i3cclk", "apll", 0, 1, div);
	} else {
		hw = clk_hw_register_fixed_factor(NULL, "i3cclk", "ahb", 0, 1, 1);
	}
	aspeed_g6_clk_data->hws[ASPEED_CLK_I3C] = hw;	

};

static void __init aspeed_g6_cc_init(struct device_node *np)
{
	struct regmap *map;
	u32 uart_clk_source = 0;
	int ret;
	int i;

	scu_g6_base = of_iomap(np, 0);
	if (!scu_g6_base)
		return;

	aspeed_g6_clk_data = kzalloc(struct_size(aspeed_g6_clk_data, hws,
				      ASPEED_G6_NUM_CLKS), GFP_KERNEL);
	if (!aspeed_g6_clk_data)
		return;

	/*
	 * This way all clocks fetched before the platform device probes,
	 * except those we assign here for early use, will be deferred.
	 */
	for (i = 0; i < ASPEED_G6_NUM_CLKS; i++)
		aspeed_g6_clk_data->hws[i] = ERR_PTR(-EPROBE_DEFER);

	map = syscon_node_to_regmap(np);
	if (IS_ERR(map)) {
		pr_err("no syscon regmap\n");
		return;
	}

	of_property_read_u32(np, "uart-clk-source", &uart_clk_source);

	if (uart_clk_source) {
		if(uart_clk_source & GENMASK(5, 0))
			regmap_update_bits(map, ASPEED_G6_CLK_SELECTION4, GENMASK(5, 0), uart_clk_source & GENMASK(5, 0));

		if(uart_clk_source & GENMASK(12, 6))
			regmap_update_bits(map, ASPEED_G6_CLK_SELECTION5, GENMASK(12, 6), uart_clk_source & GENMASK(12, 6));
	}

	/* fixed settings for RGMII/RMII clock generator */
	/* MAC1/2 RGMII 125MHz = EPLL / 8 */
	regmap_update_bits(map, ASPEED_G6_CLK_SELECTION2, GENMASK(23, 20),
			   (0x7 << 20));

	/* MAC3/4 RMII 50MHz = HCLK / 4 */
	regmap_update_bits(map, ASPEED_G6_CLK_SELECTION4, GENMASK(18, 16),
			   (0x3 << 16));

	/* BIT[31]: MAC1/2 RGMII 125M source = internal PLL
	 * BIT[28]: RGMIICK pad direction = output
	 */
	regmap_write(map, ASPEED_G6_MAC12_CLK_CTRL0,
		     BIT(31) | BIT(28) | ASPEED_G6_DEF_MAC12_DELAY_1G);
	regmap_write(map, ASPEED_G6_MAC12_CLK_CTRL1,
		     ASPEED_G6_DEF_MAC12_DELAY_100M);
	regmap_write(map, ASPEED_G6_MAC12_CLK_CTRL2,
		     ASPEED_G6_DEF_MAC12_DELAY_10M);

	/* MAC3/4 RGMII 125M source = RGMIICK pad */
	regmap_write(map, ASPEED_G6_MAC34_CLK_CTRL0,
		     ASPEED_G6_DEF_MAC34_DELAY_1G);
	regmap_write(map, ASPEED_G6_MAC34_CLK_CTRL1,
		     ASPEED_G6_DEF_MAC34_DELAY_100M);
	regmap_write(map, ASPEED_G6_MAC34_CLK_CTRL2,
		     ASPEED_G6_DEF_MAC34_DELAY_10M);

	/* MAC3/4 default pad driving strength */
	regmap_write(map, ASPEED_G6_MAC34_DRIVING_CTRL, 0x0000000a);
	
	/* RSA clock = HPLL/3 */
	regmap_update_bits(map, ASPEED_G6_CLK_SELECTION1, BIT(19), BIT(19));	
	regmap_update_bits(map, ASPEED_G6_CLK_SELECTION1, GENMASK(27, 26), (2 << 26));	

	/*
	 * We check that the regmap works on this very first access,
	 * but as this is an MMIO-backed regmap, subsequent regmap
	 * access is not going to fail and we skip error checks from
	 * this point.
	 */
	aspeed_ast2600_cc(map);	
	aspeed_g6_clk_data->num = ASPEED_G6_NUM_CLKS;
	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, aspeed_g6_clk_data);
	if (ret)
		pr_err("failed to add DT provider: %d\n", ret);

};
CLK_OF_DECLARE_DRIVER(aspeed_cc_g6, "aspeed,ast2600-scu", aspeed_g6_cc_init);
