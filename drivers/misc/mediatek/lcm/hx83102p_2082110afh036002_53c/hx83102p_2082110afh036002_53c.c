/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#define LOG_TAG "LCM"
#include "lcm_drv.h"
#ifdef BUILD_LK
#include <platform/upmu_common.h>
#include <platform/mt_gpio.h>
#include <platform/mt_i2c.h>
#include <platform/mt_pmic.h>
#include <string.h>
#else
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_gpio.h>
#include <linux/i2c.h>

#include "it6112_seq.h"
#include <asm-generic/gpio.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#endif
#include "ktz8864a.h"

#ifdef BUILD_LK
#define LCM_LOGI(string, args...)  dprintf(0, "[LK/"LOG_TAG"]"string, ##args)
#define LCM_LOGD(string, args...)  dprintf(1, "[LK/"LOG_TAG"]"string, ##args)
#else
#define LCM_LOGI(fmt, args...)  pr_debug("[KERNEL/"LOG_TAG"]"fmt, ##args)
#define LCM_LOGD(fmt, args...)  pr_debug("[KERNEL/"LOG_TAG"]"fmt, ##args)
#endif

static struct LCM_UTIL_FUNCS lcm_util;
/* extern int ktz8864a_write_bytes(unsigned char addr, unsigned char value); */
#define SET_RESET_PIN(v)	(lcm_util.set_reset_pin((v)))
#define MDELAY(n)		(lcm_util.mdelay(n))
#define UDELAY(n)		(lcm_util.udelay(n))

#define dsi_set_cmdq_V22(cmdq, cmd, count, ppara, force_update) \
		lcm_util.dsi_set_cmdq_V22(cmdq, cmd, count, ppara, force_update)
#define dsi_set_cmdq_V2(cmd, count, ppara, force_update) \
		lcm_util.dsi_set_cmdq_V2(cmd, count, ppara, force_update)
#define dsi_set_cmdq(pdata, queue_size, force_update) \
		lcm_util.dsi_set_cmdq(pdata, queue_size, force_update)
#define wrtie_cmd(cmd) lcm_util.dsi_write_cmd(cmd)
#define write_regs(addr, pdata, byte_nums) \
		lcm_util.dsi_write_regs(addr, pdata, byte_nums)
#define read_reg(cmd)	lcm_util.dsi_dcs_read_lcm_reg(cmd)
#define read_reg_v2(cmd, buffer, buffer_size) \
		lcm_util.dsi_dcs_read_lcm_reg_v2(cmd, buffer, buffer_size)

#ifndef BUILD_LK
static unsigned int GPIO_LCD_BL_EN;
static unsigned int GPIO_LCD_RST;
static struct regulator *lcm_vgp;

/* Get VTP LDO supply */
static int lcm_get_vgp_supply(struct device *dev)
{
	int ret;
	struct regulator *lcm_vgp_ldo;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	lcm_vgp_ldo = devm_regulator_get(dev, "reg-lcm");
	if (IS_ERR(lcm_vgp_ldo)) {
		ret = PTR_ERR(lcm_vgp_ldo);
		pr_notice("failed to get reg-lcm LDO, %d\n", ret);
		return ret;
	}

	pr_notice("LCM: lcm get supply ok.\n");

	ret = regulator_enable(lcm_vgp_ldo);
	/* get current voltage settings */
	ret = regulator_get_voltage(lcm_vgp_ldo);
	pr_notice("lcm LDO voltage = %d in LK stage\n", ret);

	lcm_vgp = lcm_vgp_ldo;

	return ret;
}

int lcm_vgp_supply_enable(void)
{
	int ret;
	unsigned int volt;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	if (lcm_vgp == NULL)
		return 0;

	pr_notice("LCM: set regulator voltage lcm_vgp voltage to 1.8V\n");
	/* set voltage to 1.8V */
	ret = regulator_set_voltage(lcm_vgp, 1800000, 1800000);
	if (ret != 0) {
		pr_notice("LCM: lcm failed to set lcm_vgp voltage: %d\n", ret);
		return ret;
	}

	/* get voltage settings again */
	volt = regulator_get_voltage(lcm_vgp);
	if (volt == 1800000)
		pr_notice("LCM: check regulator voltage=1800000 pass!\n");
	else
		pr_notice("LCM: check regulator voltage=1800000 fail! (voltage: %d)\n",
			volt);

	ret = regulator_enable(lcm_vgp);
	if (ret != 0) {
		pr_notice("LCM: Failed to enable lcm_vgp: %d\n", ret);
		return ret;
	}

	return ret;
}

int lcm_vgp_supply_disable(void)
{
	int ret = 0;
	unsigned int isenable;

	if (lcm_vgp == NULL)
		return 0;

	/* disable regulator */
	isenable = regulator_is_enabled(lcm_vgp);

	pr_notice("LCM: lcm query regulator enable status[%d]\n",
		isenable);

	if (isenable) {
		ret = regulator_disable(lcm_vgp);
		if (ret != 0) {
			pr_notice("LCM: lcm failed to disable lcm_vgp: %d\n",
				ret);
			return ret;
		}
		/* verify */
		isenable = regulator_is_enabled(lcm_vgp);
		if (!isenable)
			pr_notice("LCM: lcm regulator disable pass\n");
	}

	return ret;
}

/* tb8788p1 board panel GPIOs, from the dtbo "elink,lcm" node:
 *   avdd = GPIO70, dvdd = GPIO55, rst = GPIO45 (TDDI whole-chip reset),
 *   bl = GPIO18, ts_rst = GPIO158 (touch-only reset, used by the touch drv).
 * The stock kernel powers/resets the panel via these in the early
 * "elink,lcm" platform probe, BEFORE the touch probe.  This board has no
 * gpio_lcd_* properties in the main dtb, so of_get_named_gpio() fails there;
 * use the real numbers directly. */
#define LCM_GPIO_AVDD	70
#define LCM_GPIO_DVDD	55
#define LCM_GPIO_RST	45
#define LCM_GPIO_BL	18
#define LCM_GPIO_TS_RST	158
#define LCM_GPIO_EXT_PWR 86

void lcm_request_gpio_control(struct device *dev)
{
	pr_notice("[Kernel/LCM] %s enter\n", __func__);

	gpio_request(LCM_GPIO_AVDD, "lcm_avdd");
	gpio_request(LCM_GPIO_DVDD, "lcm_dvdd");
	gpio_request(LCM_GPIO_RST, "lcm_rst");
	gpio_request(LCM_GPIO_BL, "lcm_bl");
	gpio_request(LCM_GPIO_TS_RST, "lcm_ts_rst");
	gpio_request(LCM_GPIO_EXT_PWR, "lcm_ext_pwr");

	/* keep the original globals valid for code paths that reference them */
	GPIO_LCD_RST = LCM_GPIO_RST;
	GPIO_LCD_BL_EN = LCM_GPIO_BL;
}

static int lcm_driver_probe(struct device *dev, void const *data)
{
	pr_notice("[KE/LCM] %s() enter\n", __func__);

	lcm_get_vgp_supply(dev);
	lcm_request_gpio_control(dev);

	return 0;
}

static const struct of_device_id lcm_platform_of_match[] = {
	{
		.compatible = "hx,hx83102p",
		.data = 0,
	}, {
		/* sentinel */
	}
};

MODULE_DEVICE_TABLE(of, platform_of_match);

static int lcm_platform_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;

	id = of_match_node(lcm_platform_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	return lcm_driver_probe(&pdev->dev, id->data);
}

static struct platform_driver lcm_driver = {
	.probe = lcm_platform_probe,
	.driver = {
		.name = "hx83102p_fhdp_dsi_vdo_boe",
		.owner = THIS_MODULE,
		.of_match_table = lcm_platform_of_match,
	},
};

static int __init lcm_drv_init(void)
{
	pr_notice("[Kernel/LCM] %s enter__lsy\n", __func__);
	if (platform_driver_register(&lcm_driver)) {
		pr_notice("LCM: failed to register disp driver\n");
		return -ENODEV;
	}

	return 0;
}

static void __exit lcm_drv_exit(void)
{
	platform_driver_unregister(&lcm_driver);
	pr_notice("LCM: Unregister lcm driver done\n");
}

late_initcall(lcm_drv_init);
module_exit(lcm_drv_exit);
MODULE_AUTHOR("mediatek");
MODULE_DESCRIPTION("Display subsystem Driver");
MODULE_LICENSE("GPL");
#endif


#define FRAME_WIDTH             (1600)
#define FRAME_HEIGHT            (2176)
#define GPIO_OUT_ONE            1
#define GPIO_OUT_ZERO           0

/* physical size in um */
#define LCM_PHYSICAL_WIDTH		(143100)
#define LCM_PHYSICAL_HEIGHT		(238500)
#define LCM_DENSITY             (213)

#define REGFLAG_DELAY           0xFFFC
#define REGFLAG_UDELAY          0xFFFB
#define REGFLAG_END_OF_TABLE    0xFFFD
#define REGFLAG_RESET_LOW       0xFFFE
#define REGFLAG_RESET_HIGH      0xFFFF

#define LCM_ID_HX83102P         0x83

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
static struct regulator *disp_bias_pos __maybe_unused;
static struct regulator *disp_bias_neg __maybe_unused;
static int regulator_inited __maybe_unused;
#endif

static void lcm_set_gpio_output(unsigned int GPIO, unsigned int output)
{
#ifdef BUILD_LK
	mt_set_gpio_mode(GPIO, GPIO_MODE_00);
	mt_set_gpio_dir(GPIO, GPIO_DIR_OUT);
	mt_set_gpio_out(GPIO, output);
#else
	gpio_direction_output(GPIO, output);
	gpio_set_value(GPIO, output);
#endif
}

struct LCM_setting_table {
	unsigned int cmd;
	unsigned char count;
	unsigned char para_list[64];
};

/* tb8788p1: no suspend/resume tables — the stock keeps the panel running
 * across suspend and only drops the backlight; see lcm_suspend/lcm_resume. */

static void lcm_initial_registers(void)
{
	unsigned int data_array[16];

/* tb8788p1: init table aligned 1:1 with the stock kernel
 * (extracted from the original kernel binary, 61 commands:
 * panel/touch config + sleep-out(0x11) + display-on(0x29)).
 * The previous LK-derived table sent 20 extra FF82/B0/FA page-0x82
 * writes after display-on that the stock kernel never sends and
 * which break the HX83102 TDDI touch scan. */
	data_array[0] = 0x043902;
	data_array[1] = 0x2E1083B9;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x053902;
	data_array[1] = 0xFF0C67D1;
	data_array[2] = 0x00000005;
	dsi_set_cmdq(data_array, 3, 1);

	data_array[0] = 0x123902;
	data_array[1] = 0xAFFA10B1;
	data_array[2] = 0xC12B2BAF;
	data_array[3] = 0x3636256B;
	data_array[4] = 0x21223636;
	data_array[5] = 0x00000015;
	dsi_set_cmdq(data_array, 6, 1);

	data_array[0] = 0x033902;
	data_array[1] = 0x002B2BD2;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x113902;
	data_array[1] = 0x684000B2;
	data_array[2] = 0x67220080;
	data_array[3] = 0x00000022;
	data_array[4] = 0xD7201500;
	data_array[5] = 0x00000000;
	dsi_set_cmdq(data_array, 6, 1);

	data_array[0] = 0x03BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x80B21502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x113902;
	data_array[1] = 0x886488B4;
	data_array[2] = 0x68648864;
	data_array[3] = 0x01800150;
	data_array[4] = 0x00FF0058;
	data_array[5] = 0x000000FF;
	dsi_set_cmdq(data_array, 6, 1);

	data_array[0] = 0xCDE91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01BB1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x043902;
	data_array[1] = 0x8085FCBF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x093902;
	data_array[1] = 0xA80370BA;
	data_array[2] = 0x8000F283;
	data_array[3] = 0x0000000D;
	dsi_set_cmdq(data_array, 4, 1);

	data_array[0] = 0x173902;
	data_array[1] = 0x000000D3;
	data_array[2] = 0x00040100;
	data_array[3] = 0x277F0010;
	data_array[4] = 0x16167F72;
	data_array[5] = 0x10320404;
	data_array[6] = 0x00160016;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0xD7E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x163902;
	data_array[1] = 0xA91832D3;
	data_array[2] = 0x1032A908;
	data_array[3] = 0x00100010;
	data_array[4] = 0x012B1B00;
	data_array[5] = 0x012C1C7F;
	data_array[6] = 0x00000F7F;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0x00E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x183902;
	data_array[1] = 0x080200E0;
	data_array[2] = 0x2816110C;
	data_array[3] = 0x432C312C;
	data_array[4] = 0x5C5C4C48;
	data_array[5] = 0x9692786B;
	data_array[6] = 0x6663564C;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0xD8E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x183902;
	data_array[1] = 0x080200E0;
	data_array[2] = 0x2816110C;
	data_array[3] = 0x432C312C;
	data_array[4] = 0x5C5C4C48;
	data_array[5] = 0x9692786B;
	data_array[6] = 0x7263564C;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0x00E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x053902;
	data_array[1] = 0x019B01B1;
	data_array[2] = 0x00000031;
	dsi_set_cmdq(data_array, 3, 1);

	data_array[0] = 0x0B3902;
	data_array[1] = 0x1236F4CB;
	data_array[2] = 0x6C28C016;
	data_array[3] = 0x00043F85;
	dsi_set_cmdq(data_array, 4, 1);

	data_array[0] = 0x0C3902;
	data_array[1] = 0x7C0001D3;
	data_array[2] = 0x10110000;
	data_array[3] = 0x01000A00;
	dsi_set_cmdq(data_array, 4, 1);

	data_array[0] = 0x02BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x073902;
	data_array[1] = 0x33004EB4;
	data_array[2] = 0x00883311;
	dsi_set_cmdq(data_array, 3, 1);

	data_array[0] = 0x043902;
	data_array[1] = 0x0200F2BF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x00BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F3902;
	data_array[1] = 0x222323C0;
	data_array[2] = 0x0017A211;
	data_array[3] = 0x08000080;
	data_array[4] = 0x00636300;
	dsi_set_cmdq(data_array, 5, 1);

	data_array[0] = 0x093902;
	data_array[1] = 0x040400C8;
	data_array[2] = 0x13820000;
	data_array[3] = 0x000000FF;
	dsi_set_cmdq(data_array, 4, 1);

	data_array[0] = 0x043902;
	data_array[1] = 0x050407D0;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x173902;
	data_array[1] = 0x251818D5;
	data_array[2] = 0x3A2E2D24;
	data_array[3] = 0x0908093A;
	data_array[4] = 0x01000108;
	data_array[5] = 0x0B0A0B00;
	data_array[6] = 0x0002030A;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0xD7E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x173902;
	data_array[1] = 0x0D0203D5;
	data_array[2] = 0x050C0D0C;
	data_array[3] = 0x0F040504;
	data_array[4] = 0x070E0F0E;
	data_array[5] = 0x18060706;
	data_array[6] = 0x00202118;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0x00E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x173902;
	data_array[1] = 0x201818D6;
	data_array[2] = 0x2E3A3A21;
	data_array[3] = 0x0607062D;
	data_array[4] = 0x0E0F0E07;
	data_array[5] = 0x0405040F;
	data_array[6] = 0x000D0C05;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0xD7E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x173902;
	data_array[1] = 0x020D0CD6;
	data_array[2] = 0x0A030203;
	data_array[3] = 0x000B0A0B;
	data_array[4] = 0x08010001;
	data_array[5] = 0x18090809;
	data_array[6] = 0x00252418;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0x00E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x173902;
	data_array[1] = 0x021312E7;
	data_array[2] = 0x0E505002;
	data_array[3] = 0x282B1A0E;
	data_array[4] = 0x016B2C67;
	data_array[5] = 0x00000027;
	data_array[6] = 0x00001700;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0x01BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x083902;
	data_array[1] = 0x015002E7;
	data_array[2] = 0x0EF00DA0;
	dsi_set_cmdq(data_array, 3, 1);

	data_array[0] = 0x02BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x173902;
	data_array[1] = 0xFD01FAE7;
	data_array[2] = 0x27000001;
	data_array[3] = 0x00000000;
	data_array[4] = 0x00000000;
	data_array[5] = 0x00000000;
	data_array[6] = 0x00000000;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0xD7E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x073902;
	data_array[1] = 0x810000E7;
	data_array[2] = 0x00400200;
	dsi_set_cmdq(data_array, 3, 1);

	data_array[0] = 0x00E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D3902;
	data_array[1] = 0xFFFFFFD8;
	data_array[2] = 0xFFF0FFFF;
	data_array[3] = 0xFFFFFFFF;
	data_array[4] = 0x000000F0;
	dsi_set_cmdq(data_array, 5, 1);

	data_array[0] = 0x03BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x173902;
	data_array[1] = 0xAAAAAAD8;
	data_array[2] = 0xAAAAAAAA;
	data_array[3] = 0xAAAAAAAA;
	data_array[4] = 0x555555AA;
	data_array[5] = 0x55555555;
	data_array[6] = 0x00555555;
	dsi_set_cmdq(data_array, 7, 1);

	data_array[0] = 0xD7E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x033902;
	data_array[1] = 0x005555D8;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x00E91502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00BD1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02CC1502;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00110500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(120);

	data_array[0] = 0x00290500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(10);
}

static struct LCM_setting_table
	__maybe_unused lcm_deep_sleep_mode_in_setting[] = {
	{0x28, 1, {0x00} },
	{REGFLAG_DELAY, 20, {} },
	{0x10, 1, {0x00} },
	{REGFLAG_DELAY, 120, {} },
};

static struct LCM_setting_table lcm_sleep_out_setting[] = {
	{0x11, 1, {0x00} },
	{REGFLAG_DELAY, 120, {} },
	{0x29, 1, {0x00} },
	{REGFLAG_DELAY, 50, {} },
};

static struct LCM_setting_table bl_level[] = {
	{0x51, 2, {0x07, 0x87} },
	{REGFLAG_END_OF_TABLE, 0x00, {} }
};

static void push_table(void *cmdq, struct LCM_setting_table *table,
		       unsigned int count, unsigned char force_update)
{
	unsigned int i;
	unsigned int cmd;

	for (i = 0; i < count; i++) {
		cmd = table[i].cmd;
		switch (cmd) {
		case REGFLAG_DELAY:
			if (table[i].count <= 10)
				MDELAY(table[i].count);
			else
				MDELAY(table[i].count);
			break;
		case REGFLAG_UDELAY:
			UDELAY(table[i].count);
			break;
		case REGFLAG_END_OF_TABLE:
			break;
		default:
			dsi_set_cmdq_V22(cmdq, cmd, table[i].count,
					 table[i].para_list, force_update);
			break;
		}
	}
}

static void lcm_set_util_funcs(const struct LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(struct LCM_UTIL_FUNCS));
}

static void lcm_get_params(struct LCM_PARAMS *params)
{
	memset(params, 0, sizeof(struct LCM_PARAMS));

	params->type = LCM_TYPE_DSI;

	params->width = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->physical_width = LCM_PHYSICAL_WIDTH / 1000;
	params->physical_height = LCM_PHYSICAL_HEIGHT / 1000;
	params->physical_width_um = LCM_PHYSICAL_WIDTH;
	params->physical_height_um = LCM_PHYSICAL_HEIGHT;

	params->dsi.cont_clock = 1;
	params->dsi.ssc_disable = 1;
	params->dsi.mode = SYNC_PULSE_VDO_MODE;
	params->dsi.switch_mode = CMD_MODE;
	lcm_dsi_mode = SYNC_PULSE_VDO_MODE;
	LCM_LOGI("%s: lcm_dsi_mode %d\n", __func__, lcm_dsi_mode);
	params->dsi.switch_mode_enable = 0;

	/* DSI */
	/* Command mode setting */
	params->dsi.LANE_NUM = LCM_FOUR_LANE;
	/* The following defined the fomat for data coming from LCD engine. */
	params->dsi.data_format.color_order = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format = LCM_DSI_FORMAT_RGB888;

	/* Highly depends on LCD driver capability. */
	params->dsi.packet_size = 256;
	/* video mode timing */

	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;

	params->dsi.vertical_sync_active = 8;
	params->dsi.vertical_backporch = 28;
	params->dsi.vertical_frontporch = 103;
	/* params->dsi.vertical_frontporch_for_low_power = 750; */
	params->dsi.vertical_active_line = FRAME_HEIGHT;

	params->dsi.horizontal_sync_active = 14;
	params->dsi.horizontal_backporch = 28;
	params->dsi.horizontal_frontporch = 42;
	params->dsi.horizontal_active_pixel = FRAME_WIDTH;

	params->dsi.PLL_CLOCK = 746;
	params->dsi.PLL_CK_CMD = 480;

	params->dsi.CLK_HS_POST = 24; /* tb8788p1: stock value (the TIMECON3 byte-2 field) */
	params->dsi.HS_PRPR = 7; /* tb8788p1: stock value (reg byte order) */
	params->dsi.clk_lp_per_line_enable = 0;
	/*
	 * params->dsi.esd_check_enable = 0;
	 * params->dsi.customization_esd_check_enable = 0;
	 * params->dsi.lcm_esd_check_table[0].cmd = 0x0a;
	 * params->dsi.lcm_esd_check_table[0].count = 1;
	 * params->dsi.lcm_esd_check_table[0].para_list[0] = 0x9d;
	 * for ARR 2.0
	 * params->max_refresh_rate = 60;
	 * params->min_refresh_rate = 45;
	 */
}

static void lcm_init(void)
{
	pr_notice("[Kernel/LCM] %s enter (TDDI reset GPIO45 + clean table)\n",
		__func__);

	/* Power rails (already on from LK; assert to be safe). dvdd before avdd. */
	lcm_set_gpio_output(LCM_GPIO_DVDD, GPIO_OUT_ONE);
	MDELAY(2);
	lcm_set_gpio_output(LCM_GPIO_AVDD, GPIO_OUT_ONE);
	MDELAY(5);

	/* Pulse the TDDI whole-chip reset (GPIO45) to drop LK's panel state
	 * (its extra page-0x82 writes leave the HX83102 touch scan disabled),
	 * then re-init with the clean stock table. */
	lcm_set_gpio_output(LCM_GPIO_RST, GPIO_OUT_ONE);
	MDELAY(5);
	lcm_set_gpio_output(LCM_GPIO_RST, GPIO_OUT_ZERO);
	MDELAY(15);
	lcm_set_gpio_output(LCM_GPIO_RST, GPIO_OUT_ONE);
	MDELAY(120);

	lcm_initial_registers();

	lcm_set_gpio_output(LCM_GPIO_BL, GPIO_OUT_ONE);
	LCM_LOGI("[Kernel/LCM] %s exit\n", __func__);
}

static void lcm_suspend(void)
{
	LCM_LOGI("[Kernel/LCM] %s enter\n", __func__);

	/* tb8788p1: the stock keeps the panel and the it6112 bridge powered
	 * across the screen-off (GPIO snapshot proof: avdd/dvdd/ext_pwr are
	 * never toggled; only the backlight and the DSI/mmsys go down via
	 * the display framework). Cutting the rails here tears the bottom
	 * of the last frame — do nothing. */
	LCM_LOGI("[Kernel/LCM] %s exit\n", __func__);
}

/* tb8788p1: replay the stock kernel's it6112 MIPI-bridge init sequence.
 * The panel's DSI path goes through the ITE IT6112 bridge (i2c3 @ 0x56);
 * it loses its MIPI lock across suspend and the stock re-inits it at
 * resume (it6112_init). This table was captured from the stock kernel's
 * i2c traffic during a wake (1365 transactions, see it6112_seq.h). */
static void lcm_it6112_replay(void)
{
	struct i2c_adapter *adap;
	struct i2c_msg msgs[2];
	unsigned char rd_data;
	int i, ret, err_cnt = 0;

	adap = i2c_get_adapter(3);
	if (adap == NULL) {
		LCM_LOGI("[Kernel/LCM] it6112: no i2c3 adapter\n");
		return;
	}

	for (i = 0; i < IT6112_SEQ_N; i++) {
		int attempt;

		if (it6112_seq[i].rd) {
			unsigned char cmd = it6112_seq[i].d[0];

			msgs[0].addr = 0x56;
			msgs[0].flags = 0;
			msgs[0].len = 1;
			msgs[0].buf = &cmd;
			msgs[1].addr = 0x56;
			msgs[1].flags = I2C_M_RD;
			msgs[1].len = 1;
			msgs[1].buf = &rd_data;
		} else {
			unsigned char wbuf[4];
			int len = it6112_seq[i].len;
			int j;

			for (j = 0; j < len; j++)
				wbuf[j] = it6112_seq[i].d[j];
			msgs[0].addr = 0x56;
			msgs[0].flags = 0;
			msgs[0].len = len;
			msgs[0].buf = wbuf;
		}

		/* tb8788p1: the bridge may still be coming out of reset when the
		 * first i2c commands arrive (observed: single ACK error on one
		 * transfer -> panel stays dark). Retry a failed transfer a few
		 * times with a short delay instead of dropping the command. */
		for (attempt = 0; attempt < 4; attempt++) {
			if (it6112_seq[i].rd)
				ret = i2c_transfer(adap, msgs, 2);
			else
				ret = i2c_transfer(adap, msgs, 1);
			if (ret >= 0)
				break;
			UDELAY(1000);
		}
		if (ret < 0) {
			if (err_cnt < 8)
				pr_info("[Kernel/LCM] it6112: xfer %d failed %d\n",
					i, ret);
			err_cnt++;
		}
	}
	pr_info("[Kernel/LCM] it6112: replay done (%d xfers, %d failed)\n",
		IT6112_SEQ_N, err_cnt);
}

static void lcm_resume(void)
{
	pr_info("[Kernel/LCM] %s enter\n", __func__);
	LCM_LOGI("[Kernel/LCM] %s enter\n", __func__);

	/* tb8788p1: mirror the stock wake sequence exactly:
	 * 1. it6112_power_ldo_on (recovered from its runtime code):
	 *    avdd(1), 2ms, ext_pwr(1), 0.2ms, dvdd(1), 5ms, ts_rst(1), 5ms
	 * 2. the stock lcm_init resume path: whole-chip reset pulse,
	 *    150ms, then the it6112 MIPI re-init (replayed i2c sequence),
	 *    100ms, backlight. */
	lcm_set_gpio_output(LCM_GPIO_AVDD, GPIO_OUT_ONE);
	MDELAY(2);
	lcm_set_gpio_output(LCM_GPIO_EXT_PWR, GPIO_OUT_ONE);
	UDELAY(200);
	lcm_set_gpio_output(LCM_GPIO_DVDD, GPIO_OUT_ONE);
	MDELAY(5);
	lcm_set_gpio_output(LCM_GPIO_TS_RST, GPIO_OUT_ONE);
	MDELAY(5);

	lcm_set_gpio_output(LCM_GPIO_RST, GPIO_OUT_ZERO);
	MDELAY(5);
	lcm_set_gpio_output(LCM_GPIO_RST, GPIO_OUT_ONE);
	MDELAY(150);

	lcm_it6112_replay();
	MDELAY(100);

	/* tb8788p1: after a long screen-off the it6112 bridge and the TDDI
	 * lose their MIPI/display-on state; the whole-chip reset pulse above
	 * returns the panel to sleep mode, so re-issue sleep-out + display-on
	 * here or the panel stays dark (backlight only) on a long-idle wake. */
	push_table(NULL, lcm_sleep_out_setting,
		   ARRAY_SIZE(lcm_sleep_out_setting), 1);

	lcm_set_gpio_output(LCM_GPIO_BL, GPIO_OUT_ONE);
	pr_info("[Kernel/LCM] %s exit\n", __func__);
	LCM_LOGI("[Kernel/LCM] %s exit\n", __func__);
}

static unsigned int lcm_ata_check(unsigned char *buffer)
{
#ifndef BUILD_LK
	unsigned int ret = 0;
	unsigned int id[3] = {0x83, 0x11, 0x2B};
	unsigned int data_array[3];
	unsigned char read_buf[3];

	data_array[0] = 0x00033700; /* set max return size = 3 */
	dsi_set_cmdq(data_array, 1, 1);

	read_reg_v2(0x04, read_buf, 3); /* read lcm id */

	LCM_LOGI("ATA read = 0x%x, 0x%x, 0x%x\n",
		 read_buf[0], read_buf[1], read_buf[2]);

	if ((read_buf[0] == id[0]) &&
	    (read_buf[1] == id[1]) &&
	    (read_buf[2] == id[2]))
		ret = 1;
	else
		ret = 0;

	return ret;
#else
	return 1;
#endif
}

static void lcm_setbacklight_cmdq(void *handle, unsigned int level)
{
	LCM_LOGI("%s, hx83102p backlight: level = %d\n", __func__, level);

	if (level != 0)
		level = level * 4095 / 255;
	bl_level[0].para_list[0] = (level >> 8) & 0xF;
	bl_level[0].para_list[1] = level & 0xFF;

	push_table(handle, bl_level, ARRAY_SIZE(bl_level), 1);
}

static void lcm_update(unsigned int x, unsigned int y, unsigned int width,
	unsigned int height)
{
	unsigned int x0 = x;
	unsigned int y0 = y;
	unsigned int x1 = x0 + width - 1;
	unsigned int y1 = y0 + height - 1;

	unsigned char x0_MSB = ((x0 >> 8) & 0xFF);
	unsigned char x0_LSB = (x0 & 0xFF);
	unsigned char x1_MSB = ((x1 >> 8) & 0xFF);
	unsigned char x1_LSB = (x1 & 0xFF);
	unsigned char y0_MSB = ((y0 >> 8) & 0xFF);
	unsigned char y0_LSB = (y0 & 0xFF);
	unsigned char y1_MSB = ((y1 >> 8) & 0xFF);
	unsigned char y1_LSB = (y1 & 0xFF);

	unsigned int data_array[16];

#ifdef LCM_SET_DISPLAY_ON_DELAY
	lcm_set_display_on();
#endif

	data_array[0] = 0x00053902;
	data_array[1] = (x1_MSB << 24) | (x0_LSB << 16) | (x0_MSB << 8) | 0x2a;
	data_array[2] = (x1_LSB);
	dsi_set_cmdq(data_array, 3, 1);

	data_array[0] = 0x00053902;
	data_array[1] = (y1_MSB << 24) | (y0_LSB << 16) | (y0_MSB << 8) | 0x2b;
	data_array[2] = (y1_LSB);
	dsi_set_cmdq(data_array, 3, 1);

	data_array[0] = 0x002c3909;
	dsi_set_cmdq(data_array, 1, 0);
}

struct LCM_DRIVER hx83102p_2082110afh036002_53c_lcm_drv = {
	.name = "hx83102p_2082110afh036002_53c",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params = lcm_get_params,
	.init = lcm_init,
	.suspend = lcm_suspend,
	.resume = lcm_resume,
	.set_backlight_cmdq = lcm_setbacklight_cmdq,
	.ata_check = lcm_ata_check,
	.update = lcm_update,
};
