/*
 * Allwinner SoCs display driver.
 *
 * Copyright (C) 2016 Allwinner.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "dev_disp.h"
#include <linux/pm_runtime.h>
#if defined(CONFIG_DEVFREQ_DRAM_FREQ_WITH_SOFT_NOTIFY)
#include <linux/sunxi_dramfreq.h>
#endif
#include <linux/ion_sunxi.h>

#ifdef CONFIG_PM
#define CONFIG_PM_RUNTIME
#endif

#define DISP_MEM_NUM 10
struct disp_drv_info g_disp_drv;
/* alloc based on 4K byte */
#define MY_BYTE_ALIGN(x) (((x + (4*1024-1)) >> 12) << 12)

static u32 suspend_output_type[4] = {0};
/*
 * 0:normal;
 * suspend_status&1 != 0:in early_suspend;
 * suspend_status&2 != 0:in suspend;
 */
static u32 suspend_status;
/* 0:after early suspend; 1:after suspend; 2:after resume;3:after late resume */
static u32 suspend_prestep = 3;
static u32 power_status_init;

/* static unsigned int gbuffer[4096]; */
static struct info_mm g_disp_mm[10];

static struct cdev *my_cdev;
static dev_t devid;
static struct class *disp_class;
static struct device *display_dev;

static unsigned int g_disp = 0, g_enhance_mode = 0, g_cvbs_enhance_mode;
static u32 DISP_print = 0xffff;	/* print cmd which eq DISP_print */
static bool g_pm_runtime_enable;

#ifdef SUPPORT_EINK
struct disp_layer_config_inner eink_para[16];
#endif

struct disp_layer_config lyr_cfg[16];
struct disp_layer_config2 lyr_cfg2[16];
struct disp_layer_config2 lyr_cfg2_1[16];
static spinlock_t sync_finish_lock;

#ifndef CONFIG_OF
static struct sunxi_disp_mod disp_mod[] = {
	{DISP_MOD_DE, "de"},
	{DISP_MOD_LCD0, "lcd0"},
	{DISP_MOD_DSI0, "dsi0"},
#ifdef DISP_SCREEN_NUM
#if DISP_SCREEN_NUM == 2
	{DISP_MOD_LCD1, "lcd1"}
#endif
#else
#	error "DISP_SCREEN_NUM undefined!"
#endif
};

static struct resource disp_resource[] = {
};
#endif

#if defined(CONFIG_DISP2_SUNXI_COMPOSER)
int composer_init(struct disp_drv_info *p_disp_drv);
int hwc_dump(char *buf);
#endif

static void disp_shutdown(struct platform_device *pdev);
static ssize_t disp_sys_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct disp_manager *mgr = NULL;
	struct disp_device *dispdev = NULL;
	ssize_t count = 0;
	int num_screens, screen_id;
	int num_layers, layer_id;
	int num_chans, chan_id;
#if defined(CONFIG_DISP2_LCD_ESD_DETECT)
	struct disp_lcd_esd_info esd_inf;

	memset(&esd_inf, 0, sizeof(struct disp_lcd_esd_info));
#endif
	/* int hpd; */

	num_screens = bsp_disp_feat_get_num_screens();
	for (screen_id = 0; screen_id < num_screens; screen_id++) {
		u32 width = 0, height = 0;
		int fps = 0;
		struct disp_health_info info;

		mgr = disp_get_layer_manager(screen_id);
		if (mgr == NULL)
			continue;
		dispdev = mgr->device;
		if (dispdev == NULL)
			continue;
		dispdev->get_resolution(dispdev, &width, &height);
		fps = bsp_disp_get_fps(screen_id);
		bsp_disp_get_health_info(screen_id, &info);

		if (!dispdev->is_enabled(dispdev))
			continue;
		count += sprintf(buf + count, "screen %d:\n", screen_id);
		count += sprintf(buf + count, "de_rate %d hz, ref_fps:%d\n",
				 mgr->get_clk_rate(mgr),
				 dispdev->get_fps(dispdev));
		count += mgr->dump(mgr, buf + count);
		/* output */
		if (dispdev->type == DISP_OUTPUT_TYPE_LCD) {
			count += sprintf(buf + count,
				"\tlcd output\tbacklight(%3d)\tfps:%d.%d",
				dispdev->get_bright(dispdev), fps / 10,
				fps % 10);
#if defined(CONFIG_DISP2_LCD_ESD_DETECT)
			if (dispdev->get_esd_info) {
				dispdev->get_esd_info(dispdev, &esd_inf);
				count += sprintf(buf + count,
				"\tesd level(%u)\tfreq(%u)\tpos(%u)\treset(%u)",
				esd_inf.level, esd_inf.freq,
				esd_inf.esd_check_func_pos, esd_inf.rst_cnt);
			}
#endif
		} else if (dispdev->type == DISP_OUTPUT_TYPE_HDMI) {
			int mode = dispdev->get_mode(dispdev);

			count += sprintf(buf + count,
					 "\thdmi output mode(%d)\tfps:%d.%d",
					 mode, fps / 10, fps % 10);
		} else if (dispdev->type == DISP_OUTPUT_TYPE_TV) {
			int mode = dispdev->get_mode(dispdev);

			count += sprintf(buf + count,
					 "\ttv output mode(%d)\tfps:%d.%d",
					 mode, fps / 10, fps % 10);
		} else if (dispdev->type == DISP_OUTPUT_TYPE_VGA) {
			int mode = dispdev->get_mode(dispdev);

			count += sprintf(buf + count,
					 "\tvga output mode(%d)\tfps:%d.%d",
					 mode, fps / 10, fps % 10);
		}  else if (dispdev->type == DISP_OUTPUT_TYPE_VDPO) {
			int mode = dispdev->get_mode(dispdev);

			count += sprintf(buf + count,
					 "\tvdpo output mode(%d)\tfps:%d.%d",
					 mode, fps / 10, fps % 10);
		} else if (dispdev->type == DISP_OUTPUT_TYPE_EDP) {
			count += sprintf(
			    buf + count, "\tEDP output(%s) \tfps:%d.%d",
			    (dispdev->is_enabled(dispdev) == 1) ? "enable"
								: "disable",
			    fps / 10, fps % 10);
		}
		if (dispdev->type != DISP_OUTPUT_TYPE_NONE) {
			count += sprintf(buf + count, "\t%4ux%4u\n",
					 width, height);
			count += sprintf(buf + count,
					"\terr:%u\tskip:%u\tirq:%llu\tvsync:%u\tvsync_skip:%u\t\n",
					info.error_cnt, info.skip_cnt,
					info.irq_cnt, info.vsync_cnt,
					info.vsync_skip_cnt);
		}

		num_chans = bsp_disp_feat_get_num_channels(screen_id);

		/* layer info */
		for (chan_id = 0; chan_id < num_chans; chan_id++) {
			num_layers =
			    bsp_disp_feat_get_num_layers_by_chn(screen_id,
								chan_id);
			for (layer_id = 0; layer_id < num_layers; layer_id++) {
				struct disp_layer *lyr = NULL;
				struct disp_layer_config config;

				lyr = disp_get_layer(screen_id, chan_id,
						     layer_id);
				config.channel = chan_id;
				config.layer_id = layer_id;
				mgr->get_layer_config(mgr, &config, 1);
				if (lyr && (true == config.enable) && lyr->dump)
					count += lyr->dump(lyr, buf + count);
			}
		}
	}
#if defined(CONFIG_DISP2_SUNXI_COMPOSER)
	count += hwc_dump(buf + count);
#endif

	return count;
}

static ssize_t disp_sys_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	return count;
}

static DEVICE_ATTR(sys, 0660,
		   disp_sys_show, disp_sys_store);

static ssize_t disp_disp_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", g_disp);
}

static ssize_t disp_disp_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int err;
	unsigned long val;
	unsigned int num_screens;

	err = kstrtoul(buf, 10, &val);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	num_screens = bsp_disp_feat_get_num_screens();
	if (val > num_screens)
		pr_warn("Invalid value, <%d is expected!\n", num_screens);
	else
		g_disp = val;

	return count;
}

static DEVICE_ATTR(disp, 0660,
		   disp_disp_show, disp_disp_store);

static ssize_t disp_enhance_mode_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", g_enhance_mode);
}

static ssize_t disp_enhance_mode_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	int err;
	unsigned long val;

	err = kstrtoul(buf, 10, &val);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	/*
	 * mode: 0: standard; 1: vivid; 2: soft; 3: demo vivid
	 */
	if (val > 3)
		pr_warn("Invalid value, 0~3 is expected!\n");
	else {
		int num_screens = 2;
		struct disp_manager *mgr = NULL;
		struct disp_enhance *enhance = NULL;

		if (g_enhance_mode != val) {
			g_enhance_mode = val;

			num_screens = bsp_disp_feat_get_num_screens();

			if (g_disp < num_screens)
				mgr = g_disp_drv.mgr[g_disp];

			if (mgr) {
				enhance = mgr->enhance;
				if (enhance && enhance->set_mode)
#if defined(CONFIG_ARCH_SUN8IW15) || defined(CONFIG_ARCH_SUN50IW1)
					enhance->set_mode(enhance,
							 (g_enhance_mode == 2) ?
							 1 : g_enhance_mode);
					if (g_enhance_mode == 2)
						g_enhance_mode = 3;
#else
					enhance->set_mode(enhance,
							 (g_enhance_mode == 3) ?
							 1 : g_enhance_mode);
#endif

				if (enhance && enhance->demo_enable
				    && enhance->demo_disable) {
					if (g_enhance_mode == 3)
						enhance->demo_enable(enhance);
					else
						enhance->demo_disable(enhance);
				}
			}
		}
	}

	return count;
}

static DEVICE_ATTR(enhance_mode, 0660,
		   disp_enhance_mode_show, disp_enhance_mode_store);

static ssize_t disp_enhance_bright_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;
	int value = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->get_bright)
			value = enhance->get_bright(enhance);
	}

	return sprintf(buf, "%d\n", value);
}

static ssize_t disp_enhance_bright_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	int err;
	unsigned long value;
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;

	err = kstrtoul(buf, 10, &value);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->set_bright)
			enhance->set_bright(enhance, value);
	}

	return count;
}
static DEVICE_ATTR(enhance_bright, 0660,
	disp_enhance_bright_show, disp_enhance_bright_store);

static ssize_t disp_enhance_saturation_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;
	int value = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->get_saturation)
			value = enhance->get_saturation(enhance);
	}

	return sprintf(buf, "%d\n", value);
}

static ssize_t disp_enhance_saturation_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	int err;
	unsigned long value;
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;

	err = kstrtoul(buf, 10, &value);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->set_saturation)
			enhance->set_saturation(enhance, value);
	}

	return count;
}
static DEVICE_ATTR(enhance_saturation, 0660,
	disp_enhance_saturation_show, disp_enhance_saturation_store);

static ssize_t disp_enhance_contrast_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;
	int value = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->get_contrast)
			value = enhance->get_contrast(enhance);
	}

	return sprintf(buf, "%d\n", value);
}

static ssize_t disp_enhance_contrast_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	int err;
	unsigned long value;
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;

	err = kstrtoul(buf, 10, &value);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->set_contrast)
			enhance->set_contrast(enhance, value);
	}

	return count;
}
static DEVICE_ATTR(enhance_contrast, 0660,
	disp_enhance_contrast_show, disp_enhance_contrast_store);

static ssize_t disp_enhance_edge_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;
	int value = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->get_edge)
			value = enhance->get_edge(enhance);
	}

	return sprintf(buf, "%d\n", value);
}

static ssize_t disp_enhance_edge_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	int err;
	unsigned long value;
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;

	err = kstrtoul(buf, 10, &value);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->set_edge)
			enhance->set_edge(enhance, value);
	}

	return count;
}
static DEVICE_ATTR(enhance_edge, 0660,
	disp_enhance_edge_show, disp_enhance_edge_store);

static ssize_t disp_enhance_detail_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;
	int value = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->get_detail)
			value = enhance->get_detail(enhance);
	}

	return sprintf(buf, "%d\n", value);
}

static ssize_t disp_enhance_detail_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	int err;
	unsigned long value;
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;

	err = kstrtoul(buf, 10, &value);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->set_detail)
			enhance->set_detail(enhance, value);
	}

	return count;
}
static DEVICE_ATTR(enhance_detail, 0660,
	disp_enhance_detail_show, disp_enhance_detail_store);

static ssize_t disp_enhance_denoise_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;
	int value = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->get_denoise)
			value = enhance->get_denoise(enhance);
	}

	return sprintf(buf, "%d\n", value);
}

static ssize_t disp_enhance_denoise_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	int err;
	unsigned long value;
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_enhance *enhance = NULL;

	err = kstrtoul(buf, 10, &value);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr) {
		enhance = mgr->enhance;
		if (enhance && enhance->set_denoise)
			enhance->set_denoise(enhance, value);
	}

	return count;
}
static DEVICE_ATTR(enhance_denoise, 0660,
	disp_enhance_denoise_show, disp_enhance_denoise_store);

static ssize_t disp_cvbs_enhance_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", g_cvbs_enhance_mode);
}

static ssize_t disp_cvbs_enhance_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	int err;
	unsigned long val;

	err = kstrtoul(buf, 10, &val);
	if (val > 2) {
		pr_warn("Invalid value, 0/1/2 is expected!\n");
	} else {
		int num_screens = 0;
		unsigned int disp;
		struct disp_device *ptv = NULL;

		g_cvbs_enhance_mode = val;
		num_screens = bsp_disp_feat_get_num_screens();

		for (disp = 0; disp < num_screens; disp++) {
			ptv = disp_device_find(disp, DISP_OUTPUT_TYPE_TV);
			if (ptv && ptv->set_enhance_mode)
				ptv->set_enhance_mode(ptv, g_cvbs_enhance_mode);
		}
	}

	return count;
}

static DEVICE_ATTR(cvbs_enhacne_mode, 0660,
		   disp_cvbs_enhance_show, disp_cvbs_enhance_store);

static ssize_t disp_runtime_enable_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%d\n", g_pm_runtime_enable);
}

static ssize_t disp_runtime_enable_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	int err;
	unsigned long val;

	err = kstrtoul(buf, 10, &val);
	if (val > 1)
		pr_warn("Invalid value, 0/1 is expected!\n");
	else
		g_pm_runtime_enable = val;

	return count;
}

static DEVICE_ATTR(runtime_enable, 0660,
		   disp_runtime_enable_show, disp_runtime_enable_store);
static ssize_t disp_color_temperature_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_device *dispdev = NULL;
	int value = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr && mgr->device) {
		dispdev = mgr->device;
		if (dispdev->get_color_temperature)
			value = dispdev->get_color_temperature(dispdev);
	}

	return sprintf(buf, "%d\n", value);
}

static ssize_t disp_color_temperature_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	int err;
	long value;
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_device *dispdev = NULL;

	err = kstrtol(buf, 10, &value);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	if ((value > 256) || (value < -256)) {
		pr_warn("value shoud in range [-256,256]\n");
		value = (value > 256) ? 256 : value;
		value = (value < -256) ? -256 : value;
	}

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr && mgr->device) {
		dispdev = mgr->device;
		if (dispdev->set_color_temperature)
			value = dispdev->set_color_temperature(dispdev, value);
	}

	return count;
}

static DEVICE_ATTR(color_temperature, 0660,
	disp_color_temperature_show, disp_color_temperature_store);


static ssize_t disp_boot_para_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
#if DISP_SCREEN_NUM > 1
		return sprintf(buf, "disp_para=%x init_disp=%x tv_vdid=%x fb_base=0x%x disp_config0=%d,%u - %d,%d,%d,%d disp_config1=%d,%u - %d,%d,%d,%d\n",
		disp_boot_para_parse("boot_disp"),
		disp_boot_para_parse("init_disp"),
		disp_boot_para_parse("tv_vdid"),
		disp_boot_para_parse("fb_base"),
		g_disp_drv.disp_init.output_type[0], g_disp_drv.disp_init.output_mode[0],
		g_disp_drv.disp_init.output_format[0], g_disp_drv.disp_init.output_bits[0],
		g_disp_drv.disp_init.output_cs[0], g_disp_drv.disp_init.output_eotf[0],
		g_disp_drv.disp_init.output_type[1], g_disp_drv.disp_init.output_mode[1],
		g_disp_drv.disp_init.output_format[1], g_disp_drv.disp_init.output_bits[1],
		g_disp_drv.disp_init.output_cs[1], g_disp_drv.disp_init.output_eotf[1]);
#else
		return sprintf(buf, "disp_para=%x init_disp=%x tv_vdid=%x fb_base=0x%x disp_config0=%d,%u - %d,%d,%d,%d\n",
		disp_boot_para_parse("boot_disp"),
		disp_boot_para_parse("init_disp"),
		disp_boot_para_parse("tv_vdid"),
		disp_boot_para_parse("fb_base"),
		g_disp_drv.disp_init.output_type[0], g_disp_drv.disp_init.output_mode[0],
		g_disp_drv.disp_init.output_format[0], g_disp_drv.disp_init.output_bits[0],
		g_disp_drv.disp_init.output_cs[0], g_disp_drv.disp_init.output_eotf[0]);
#endif
}

static ssize_t disp_xres_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_device *dispdev = NULL;
	u32 width = 0, height;

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr && mgr->device) {
		dispdev = mgr->device;
		if (dispdev->get_resolution)
			dispdev->get_resolution(dispdev, &width, &height);
	}

	return sprintf(buf, "%d\n", width);
}

static ssize_t disp_yres_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_device *dispdev = NULL;
	u32 width = 0, height = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (mgr && mgr->device) {
		dispdev = mgr->device;
		if (dispdev->get_resolution)
			dispdev->get_resolution(dispdev, &width, &height);
	}

	return sprintf(buf, "%d\n", height);
}

/**
 * @name       :disp_draw_colorbar
 * @brief      :draw colorbar using DE's LAYER MODE
 * @param[IN]  :disp:screen index
 * @return     :0 if success
 */
int disp_draw_colorbar(u32 disp)
{
	struct disp_manager *mgr = NULL;
	struct disp_layer_config config[4];
	unsigned int i = 0;
	unsigned int width = 0, height = 0, num_screens;
	int ret = -1;

	num_screens = bsp_disp_feat_get_num_screens();
	if (disp < num_screens)
		mgr = g_disp_drv.mgr[disp];
	else
		return ret;

	if (mgr && mgr->device && mgr->device->get_resolution)
		mgr->device->get_resolution(mgr->device, &width, &height);
	else
		return ret;

	memset(config, 0, 4 * sizeof(struct disp_layer_config));
	for (i = 0; i < 4; ++i) {
		config[i].channel = 1;
		config[i].layer_id = i;
		config[i].enable = 1;
		config[i].info.zorder = 16;
		config[i].info.mode = LAYER_MODE_COLOR;
		config[i].info.fb.format = DISP_FORMAT_ARGB_8888;
		config[i].info.screen_win.width = width / 4;
		config[i].info.screen_win.height = height;
		config[i].info.screen_win.x = (width / 4) * i;
		config[i].info.screen_win.y = 0;
		config[i].info.fb.crop.x =
		    ((long long)(config[i].info.screen_win.x) << 32);
		config[i].info.fb.crop.y =
		    ((long long)(config[i].info.screen_win.y) << 32);
		config[i].info.fb.crop.width =
		    ((long long)(config[i].info.screen_win.width) << 32);
		config[i].info.fb.crop.height =
		    ((long long)(config[i].info.screen_win.height) << 32);
	}
	config[0].info.color = 0xffff0000; /*red*/
	config[1].info.color = 0xff00ff00; /*green*/
	config[2].info.color = 0xff0000ff; /*blue*/
	config[3].info.color = 0xffffff00; /*yellow*/

	if (mgr->set_layer_config)
		ret = mgr->set_layer_config(mgr, config, 4);

	return ret;
}

static ssize_t disp_colorbar_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	int err;
	unsigned int val;
	unsigned int num_screens;
	struct disp_manager *mgr = NULL;

	err = kstrtou32(buf, 10, &val);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	num_screens = bsp_disp_feat_get_num_screens();

	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	/*val:*/
	/*0:DE-->tcon-->other interface*/
	/*1-7:tcon or edp or other device's builtin patten*/
	/*for tcon:*/
	/*1:color bar*/
	/*2:grayscale check*/
	/*3:black and white check*/
	/*4:all 0*/
	/*5:all 1*/
	/*6:reserve*/
	/*7:Gridding*/
	/*for edp:*/
	/*1:colorbar*/
	/*2:mosaic*/
	if (val == 8) {
		disp_draw_colorbar(g_disp);
		if (mgr && mgr->device && mgr->device->show_builtin_patten)
			mgr->device->show_builtin_patten(mgr->device, 0);
	} else {
		if (mgr && mgr->device && mgr->device->show_builtin_patten)
			mgr->device->show_builtin_patten(mgr->device, val);
	}

	return count;
}

static ssize_t disp_capture_dump_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct file *pfile;
	mm_segment_t old_fs;
	ssize_t bw;
	dma_addr_t phy_addr = 0;
	void *buf_addr_vir = NULL;
	struct disp_capture_info cptr_info;
	unsigned int size = 0, width = 0, height = 0, num_screens = 0;
	struct disp_manager *mgr = NULL;
	char *image_name = NULL;
	int ret = -1, cs = DISP_CSC_TYPE_RGB;
	loff_t pos = 0;
	struct bmp_header bmp_header;

	num_screens = bsp_disp_feat_get_num_screens();

	if (g_disp < num_screens)
		mgr = g_disp_drv.mgr[g_disp];

	if (!mgr || !mgr->device || !mgr->cptr)
		goto OUT;

	memset(&cptr_info, 0, sizeof(struct disp_capture_info));

	image_name = kmalloc(count, GFP_KERNEL | __GFP_ZERO);
	if (!image_name) {
		__wrn("kmalloc image name fail!\n");
		goto OUT;
	}
	strncpy(image_name, buf, count);
	image_name[count - 1] = '\0';

	old_fs = get_fs();
	set_fs(get_ds());
	pfile = filp_open(image_name, O_RDWR | O_CREAT | O_EXCL, 0755);
	set_fs(old_fs);
	if (IS_ERR(pfile)) {
		__wrn("%s, open %s err\n", __func__, image_name);
		goto FREE;
	}

	if (mgr->device->get_resolution)
		ret = mgr->device->get_resolution(mgr->device, &width, &height);
	if (ret) {
		__wrn("Get resolution fail!\n");
		goto FILE_CLOSE;
	}

	cptr_info.out_frame.size[0].width = width;
	cptr_info.out_frame.size[0].height = height;
	cptr_info.window.width = width;
	cptr_info.window.height = height;
	cptr_info.out_frame.crop.width = width;
	cptr_info.out_frame.crop.height = height;
	if (strstr(image_name, ".bmp"))
		cptr_info.out_frame.format = DISP_FORMAT_ARGB_8888;
	else if (strstr(image_name, ".yuv420_p"))
		cptr_info.out_frame.format = DISP_FORMAT_YUV420_P;
	else if (strstr(image_name, ".yuv420_sp_uvuv"))
		cptr_info.out_frame.format = DISP_FORMAT_YUV420_SP_UVUV;
	else if (strstr(image_name, ".yuv420_sp_vuvu"))
		cptr_info.out_frame.format = DISP_FORMAT_YUV420_SP_VUVU;
	else if (strstr(image_name, ".argb8888"))
		cptr_info.out_frame.format = DISP_FORMAT_ARGB_8888;
	else if (strstr(image_name, ".abgr8888"))
		cptr_info.out_frame.format = DISP_FORMAT_ABGR_8888;
	else if (strstr(image_name, ".rgb888"))
		cptr_info.out_frame.format = DISP_FORMAT_RGB_888;
	else if (strstr(image_name, ".bgr888"))
		cptr_info.out_frame.format = DISP_FORMAT_BGR_888;
	else if (strstr(image_name, ".rgba8888"))
		cptr_info.out_frame.format = DISP_FORMAT_RGBA_8888;
	else if (strstr(image_name, ".bgra8888"))
		cptr_info.out_frame.format = DISP_FORMAT_BGRA_8888;
	else {
		if (mgr->device->get_input_csc)
			cs = mgr->device->get_input_csc(mgr->device);
		if (cs == DISP_CSC_TYPE_RGB)
			cptr_info.out_frame.format = DISP_FORMAT_ARGB_8888;
		else
			cptr_info.out_frame.format = DISP_FORMAT_YUV420_P;
	}

	size = width * height * 4;

	buf_addr_vir = disp_malloc(size, (void *)&phy_addr);
	if (!phy_addr || !buf_addr_vir) {
		__wrn("%s, disp_malloc phy_addr err\n", __func__);
		goto FILE_CLOSE;
	}

	cptr_info.out_frame.addr[0] = (unsigned long)phy_addr;
	cptr_info.out_frame.addr[1] =
	    cptr_info.out_frame.addr[0] + width * height;
	cptr_info.out_frame.addr[2] =
	    cptr_info.out_frame.addr[1] + width * height / 4;

	ret = mgr->cptr->start(mgr->cptr);
	if (ret) {
		mgr->cptr->stop(mgr->cptr);
		goto FREE_DMA;
	}

	ret = mgr->cptr->commmit(mgr->cptr, &cptr_info);
	if (ret) {
		mgr->cptr->stop(mgr->cptr);
		goto FREE_DMA;
	}
	disp_delay_ms(1000);
	ret = mgr->cptr->stop(mgr->cptr);
	if (ret)
		goto FREE_DMA;

	if (strstr(image_name, ".bmp")) {
		memset(&bmp_header, 0, sizeof(struct bmp_header));
		bmp_header.signature[0] = 'B';
		bmp_header.signature[1] = 'M';
		bmp_header.data_offset = sizeof(struct bmp_header);
		bmp_header.file_size = bmp_header.data_offset + size;
		bmp_header.size = sizeof(struct bmp_header) - 14;
		bmp_header.width = width;
		bmp_header.height = -height;
		bmp_header.planes = 1;
		bmp_header.bit_count = 32;
		bmp_header.image_size = size;
		old_fs = get_fs();
		set_fs(get_ds());
		bw = vfs_write(pfile, (const char *)&bmp_header,
			       sizeof(struct bmp_header), &pos);
		set_fs(old_fs);
		pos = sizeof(struct bmp_header);
	}

	old_fs = get_fs();
	set_fs(get_ds());
	bw = vfs_write(pfile, (char *)buf_addr_vir, size, &pos);
	if (unlikely(bw != size))
		__wrn("%s, write %s err at byte offset %llu\n", __func__,
		      image_name, pfile->f_pos);

	set_fs(old_fs);
FREE_DMA:
	disp_free((void *)buf_addr_vir, (void *)phy_addr, size);
FILE_CLOSE:
	filp_close(pfile, NULL);
FREE:
	kfree(image_name);
	image_name = NULL;
OUT:
	return count;
}

static DEVICE_ATTR(boot_para, 0660, disp_boot_para_show, NULL);
static DEVICE_ATTR(xres, 0660, disp_xres_show, NULL);
static DEVICE_ATTR(yres, 0660, disp_yres_show, NULL);
static DEVICE_ATTR(colorbar, 0660, NULL, disp_colorbar_store);
static DEVICE_ATTR(capture_dump, 0660, NULL, disp_capture_dump_store);

static struct attribute *disp_attributes[] = {
	&dev_attr_sys.attr,
	&dev_attr_disp.attr,
	&dev_attr_enhance_mode.attr,
	&dev_attr_cvbs_enhacne_mode.attr,
	&dev_attr_runtime_enable.attr,
	&dev_attr_enhance_bright.attr,
	&dev_attr_enhance_saturation.attr,
	&dev_attr_enhance_contrast.attr,
	&dev_attr_enhance_edge.attr,
	&dev_attr_enhance_detail.attr,
	&dev_attr_enhance_denoise.attr,
	&dev_attr_color_temperature.attr,
	&dev_attr_boot_para.attr,
	&dev_attr_xres.attr,
	&dev_attr_yres.attr,
	&dev_attr_colorbar.attr,
	&dev_attr_capture_dump.attr,
	NULL
};

static struct attribute_group disp_attribute_group = {
	.name = "attr",
	.attrs = disp_attributes
};

unsigned int disp_boot_para_parse(const char *name)
{
	unsigned int value = 0;

	if (of_property_read_u32(g_disp_drv.dev->of_node, name, &value) < 0)
		__inf("of_property_read disp.%s fail\n", name);

	__inf("[DISP] %s:0x%x\n", name, value);
	return value;
}
EXPORT_SYMBOL(disp_boot_para_parse);

unsigned int disp_boot_para_parse_array(const char *name, unsigned int *value,
							  unsigned int count)
{
	unsigned int ret = 0;

	ret = of_property_read_u32_array(g_disp_drv.dev->of_node, name,
							  value, count);
	if (ret)
		__wrn("of_property_read_array disp.%s fail\n", name);

	return ret;
}
EXPORT_SYMBOL(disp_boot_para_parse_array);


const char *disp_boot_para_parse_str(const char *name)
{
	const char *str;

	if (!of_property_read_string(g_disp_drv.dev->of_node, name, &str))
		return str;

	__inf("of_property_read_string disp.%s fail\n", name);

	return NULL;
}
EXPORT_SYMBOL(disp_boot_para_parse_str);

static s32 parser_disp_init_para(const struct device_node *np,
				 struct disp_init_para *init_para)
{
	int value;
	int i;

	memset(init_para, 0, sizeof(struct disp_init_para));

	if (of_property_read_u32(np, "disp_init_enable", &value) < 0) {
		__wrn("of_property_read disp_init.disp_init_enable fail\n");
		return -1;
	}
	init_para->b_init = value;

	if (of_property_read_u32(np, "disp_mode", &value) < 0) {
		__wrn("of_property_read disp_init.disp_mode fail\n");
		return -1;
	}
	init_para->disp_mode = value;

	/* screen0 */
	if (of_property_read_u32(np, "screen0_output_type", &value) < 0) {
		__wrn("of_property_read disp_init.screen0_output_type fail\n");
		return -1;
	}
	if (value == 0) {
		init_para->output_type[0] = DISP_OUTPUT_TYPE_NONE;
	} else if (value == 1) {
		init_para->output_type[0] = DISP_OUTPUT_TYPE_LCD;
	} else if (value == 2) {
		init_para->output_type[0] = DISP_OUTPUT_TYPE_TV;
	} else if (value == 3) {
		init_para->output_type[0] = DISP_OUTPUT_TYPE_HDMI;
	} else if (value == 4) {
		init_para->output_type[0] = DISP_OUTPUT_TYPE_VGA;
	} else if (value == 5) {
		init_para->output_type[0] = DISP_OUTPUT_TYPE_VDPO;
	} else if (value == 6) {
		init_para->output_type[0] = DISP_OUTPUT_TYPE_EDP;
	} else {
		__wrn("invalid screen0_output_type %d\n",
		      init_para->output_type[0]);
		return -1;
	}

	if (of_property_read_u32(np, "screen0_output_mode", &value) < 0) {
		__wrn("of_property_read disp_init.screen0_output_mode fail\n");
		return -1;
	}

	if (init_para->output_type[0] != DISP_OUTPUT_TYPE_NONE &&
		init_para->output_type[0] != DISP_OUTPUT_TYPE_LCD)
		init_para->output_mode[0] = value;

	if (of_property_read_u32(np, "screen0_output_format", &value) < 0) {
		__inf("of_property_read screen0_output_format fail\n");
	} else {
		init_para->output_format[0] = value;
		init_para->using_device_config[0] = true;
	}

	if (of_property_read_u32(np, "screen0_output_bits", &value) < 0) {
		__inf("of_property_read screen0_output_bits fail\n");
	} else {
		init_para->output_bits[0] = value;
		init_para->using_device_config[0] = true;
	}

	if (of_property_read_u32(np, "screen0_output_eotf", &value) < 0) {
		__inf("of_property_read screen0_output_eotf fail\n");
	} else {
		init_para->output_eotf[0] = value;
		init_para->using_device_config[0] = true;
	}

	if (of_property_read_u32(np, "screen0_output_cs", &value) < 0) {
		__inf("of_property_read screen0_output_cs fail\n");
	} else {
		init_para->output_cs[0] = value;
		init_para->using_device_config[0] = true;
	}

	if (of_property_read_u32(np, "screen0_output_dvi_hdmi", &value) < 0) {
		__inf("of_property_read screen0_output_dvi_hdmi fail\n");
	} else {
		init_para->output_dvi_hdmi[0] = value;
		init_para->using_device_config[0] = true;
	}

	if (of_property_read_u32(np, "screen0_output_range", &value) < 0) {
		__inf("of_property_read screen0_output_range fail\n");
	} else {
		init_para->output_range[0] = value;
		init_para->using_device_config[0] = true;
	}

	if (of_property_read_u32(np, "screen0_output_scan", &value) < 0) {
		__inf("of_property_read screen0_output_scan fail\n");
	} else {
		init_para->output_scan[0] = value;
		init_para->using_device_config[0] = true;
	}

	if (of_property_read_u32(np, "screen0_output_aspect_ratio", &value) < 0) {
		__inf("of_property_read screen0_output_aspect_ratio fail\n");
	} else {
		init_para->output_aspect_ratio[0] = value;
		init_para->using_device_config[0] = true;
	}

#if DISP_SCREEN_NUM > 1
		/* screen1 */
		if (of_property_read_u32(np,
					 "screen1_output_type",
					 &value) < 0) {
			__wrn("of_property_read screen1_output_type fail\n");
			return -1;
		}
		if (value == 0) {
			init_para->output_type[1] = DISP_OUTPUT_TYPE_NONE;
		} else if (value == 1) {
			init_para->output_type[1] = DISP_OUTPUT_TYPE_LCD;
		} else if (value == 2) {
			init_para->output_type[1] = DISP_OUTPUT_TYPE_TV;
		} else if (value == 3) {
			init_para->output_type[1] = DISP_OUTPUT_TYPE_HDMI;
		} else if (value == 4) {
			init_para->output_type[1] = DISP_OUTPUT_TYPE_VGA;
		} else if (value == 5) {
			init_para->output_type[1] = DISP_OUTPUT_TYPE_VDPO;
		} else if (value == 6) {
			init_para->output_type[1] = DISP_OUTPUT_TYPE_EDP;
		} else {
			__wrn("invalid screen1_output_type %d\n",
			      init_para->output_type[1]);
			return -1;
		}

		if (of_property_read_u32(np, "screen1_output_mode", &value) < 0)
			__inf
			    ("of_property_read screen1_output_mode fail\n");
		if (init_para->output_type[1] != DISP_OUTPUT_TYPE_NONE &&
		    init_para->output_type[1] != DISP_OUTPUT_TYPE_LCD)
			init_para->output_mode[1] = value;

		if (of_property_read_u32(np,
					 "screen1_output_format", &value) < 0) {
			__inf("of_property_read screen1_output_format fail\n");
		} else {
			init_para->output_format[1] = value;
			init_para->using_device_config[1] = true;
		}

		if (of_property_read_u32(np,
					 "screen1_output_bits", &value) < 0) {
			__inf("of_property_read screen1_output_bits fail\n");
		} else {
			init_para->output_bits[1] = value;
			init_para->using_device_config[1] = true;
		}

		if (of_property_read_u32(np,
					 "screen1_output_eotf", &value) < 0) {
			__inf("of_property_read screen1_output_eotf fail\n");
		} else {
			init_para->output_eotf[1] = value;
			init_para->using_device_config[1] = true;
		}

		if (of_property_read_u32(np, "screen1_output_cs", &value) < 0) {
			__inf("of_property_read screen1_output_cs fail\n");
		} else {
			init_para->output_cs[1] = value;
			init_para->using_device_config[1] = true;
		}

		if (of_property_read_u32(np, "screen1_output_dvi_hdmi", &value) < 0) {
			__inf(
			    "of_property_read screen1_output_dvi_hdmi fail\n");
		} else {
			init_para->output_dvi_hdmi[1] = value;
			init_para->using_device_config[1] = true;
		}

		if (of_property_read_u32(np, "screen1_output_range", &value) < 0) {
			__inf("of_property_read screen1_output_range fail\n");
		} else {
			init_para->output_range[1] = value;
			init_para->using_device_config[1] = true;
		}

		if (of_property_read_u32(np, "screen1_output_scan", &value) < 0) {
			__inf("of_property_read screen1_output_scan fail\n");
		} else {
			init_para->output_scan[1] = value;
			init_para->using_device_config[1] = true;
		}

		if (of_property_read_u32(np, "screen1_output_aspect_ratio", &value) < 0) {
			__inf("read screen1_output_aspect_ratio fail\n");
		} else {
			init_para->output_aspect_ratio[1] = value;
			init_para->using_device_config[1] = true;
		}
#endif
	/* fb0 */
	init_para->buffer_num[0] = 2;
	if (of_property_read_u32(np, "fb0_buffer_num", &value) == 0)
		init_para->buffer_num[0] = value;

	if (of_property_read_u32(np, "fb0_format", &value) < 0) {
		__wrn("of_property_read disp_init.fb0_format fail\n");
		return -1;
	}
	init_para->format[0] = value;

	if (of_property_read_u32(np, "fb0_width", &value) < 0) {
		__wrn("of_property_read fb0_width fail\n");
		return -1;
	}
	init_para->fb_width[0] = value;

	if (of_property_read_u32(np, "fb0_height", &value) < 0) {
		__wrn("of_property_read fb0_height fail\n");
		return -1;
	}
	init_para->fb_height[0] = value;

	/* fb1 */
#if DISP_SCREEN_NUM > 1
		init_para->buffer_num[1] = 2;

		if (of_property_read_u32(np, "fb1_buffer_num", &value) == 0)
			init_para->buffer_num[1] = value;

		if (of_property_read_u32(np, "fb1_format", &value) == 0)
			init_para->format[1] = value;

		if (of_property_read_u32(np, "fb1_width", &value) == 0)
			init_para->fb_width[1] = value;

		if (of_property_read_u32(np, "fb1_height", &value) == 0)
			init_para->fb_height[1] = value;
#endif

	__inf("====display init para begin====\n");
	__inf("b_init:%d\n", init_para->b_init);
	__inf("disp_mode:%d\n\n", init_para->disp_mode);
	for (i = 0; i < DISP_SCREEN_NUM; i++) {
		__inf("output_type[%d]:%d\n", i, init_para->output_type[i]);
		__inf("output_mode[%d]:%d\n", i, init_para->output_mode[i]);
	}
	for (i = 0; i < DISP_SCREEN_NUM; i++) {
		__inf("buffer_num[%d]:%d\n", i, init_para->buffer_num[i]);
		__inf("format[%d]:%d\n", i, init_para->format[i]);
		__inf("fb_width[%d]:%d\n", i, init_para->fb_width[i]);
		__inf("fb_height[%d]:%d\n", i, init_para->fb_height[i]);
	}
	__inf("====display init para end====\n");

	return 0;
}

void *disp_malloc(u32 num_bytes, void *phys_addr)
{
	u32 actual_bytes;
	void *address = NULL;

	if (num_bytes != 0) {
		actual_bytes = MY_BYTE_ALIGN(num_bytes);

		address =
		    dma_alloc_coherent(g_disp_drv.dev, actual_bytes,
				       (dma_addr_t *) phys_addr, GFP_KERNEL);
		if (address) {
			__inf
			    ("dma_alloc_coherent ok, address=0x%p, size=0x%x\n",
			     (void *)(*(unsigned long *)phys_addr), num_bytes);
			return address;
		}

		__wrn("dma_alloc_coherent fail, size=0x%x\n", num_bytes);
		return NULL;
	}

	__wrn("%s size is zero\n", __func__);

	return NULL;
}

void disp_free(void *virt_addr, void *phys_addr, u32 num_bytes)
{
	u32 actual_bytes;

	actual_bytes = MY_BYTE_ALIGN(num_bytes);
	if (phys_addr && virt_addr)
		dma_free_coherent(g_disp_drv.dev, actual_bytes, virt_addr,
				  (dma_addr_t)phys_addr);
}

#if defined(CONFIG_ION_SUNXI)
static int init_disp_ion_mgr(struct disp_ion_mgr *ion_mgr)
{
	if (NULL == ion_mgr) {
		__wrn("input param is null\n");
		return -EINVAL;
	}

	mutex_init(&(ion_mgr->mlock));

	mutex_lock(&(ion_mgr->mlock));
	INIT_LIST_HEAD(&(ion_mgr->ion_list));
	ion_mgr->client = sunxi_ion_client_create("ion_disp2");
	if (IS_ERR_OR_NULL(ion_mgr->client)) {
		mutex_unlock(&(ion_mgr->mlock));
		__wrn("disp_ion client create failed!!");
		return -ENOMEM;
	} else {
		__debug("init ion manager for disp ok\n");
	}
	mutex_unlock(&(ion_mgr->mlock));

	return 0;
}

static int __disp_ion_alloc_coherent(struct ion_client *client,
				     struct disp_ion_mem *mem)
{
	unsigned int flags = ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC;

	if (IS_ERR_OR_NULL(client) || (NULL == mem)) {
		__wrn("input param is null\n");
		return -1;
	}

#if defined(CONFIG_SUNXI_IOMMU)
	mem->handle = ion_alloc(client, mem->size, 0,
				(1 << ION_HEAP_TYPE_SYSTEM), flags);
#else
	mem->handle = ion_alloc(client, mem->size, PAGE_SIZE,
				 ((1 << ION_HEAP_TYPE_CARVEOUT) |
				 (1 << ION_HEAP_TYPE_DMA)),
				flags);
#endif
	if (IS_ERR_OR_NULL(mem->handle)) {
		__wrn("ion_alloc failed, size=%u 0x%p!\n", (unsigned int)mem->size, mem->handle);
		return -2;
	}
	mem->vaddr = ion_map_kernel(client, mem->handle);
	if (IS_ERR_OR_NULL(mem->vaddr)) {
		__wrn("ion_map_kernel failed!!\n");
		goto err_map_kernel;
	}

	__debug("ion map kernel, vaddr=0x%p\n", mem->vaddr);
#ifndef CONFIG_SUNXI_IOMMU
	mem->p_item = kmalloc(sizeof(struct dmabuf_item), GFP_KERNEL);
	if (ion_phys(client, mem->handle, (ion_phys_addr_t *)&mem->p_item->dma_addr,
		     &mem->size)) {
		__wrn("ion_phys failed!!\n");
		goto err_phys;
	}
#endif
#ifdef CONFIG_SUNXI_IOMMU
	mem->p_item = disp_dma_map(ion_share_dma_buf_fd2(client, mem->handle));
	if (!mem->p_item)
		goto err_phys;
#else
	mem->p_item->fd = ion_share_dma_buf_fd2(client, mem->handle);
	mem->p_item->buf = dma_buf_get(mem->p_item->fd);
	if (IS_ERR(mem->p_item->buf)) {
		DE_WRN("Get dmabuf of fd %d fail!\n", mem->p_item->fd);
		goto err_phys;
	}
#endif
	return 0;

err_phys:
	ion_unmap_kernel(client, mem->handle);
err_map_kernel:
	ion_free(client, mem->handle);
	return -ENOMEM;
}

static void __disp_ion_free_coherent(struct ion_client *client,
				     struct disp_ion_mem *mem)
{
	if (IS_ERR_OR_NULL(client) || IS_ERR_OR_NULL(mem->handle) ||
	    IS_ERR_OR_NULL(mem->vaddr)) {
		__wrn("input param is null\n");
		return;
	}

	mem->p_item->buf->ops->release(mem->p_item->buf);
	ion_unmap_kernel(client, mem->handle);
	ion_free(client, mem->handle);

#ifdef CONFIG_SUNXI_IOMMU
	disp_dma_unmap(mem->p_item);
#else
	dma_buf_put(mem->p_item->buf);
	kfree(mem->p_item);
#endif
	return;
}

void disp_ion_flush_cache(void *startAddr, int size)
{
	struct sunxi_cache_range range;

	range.start = (unsigned long)startAddr;
	range.end = (unsigned long)startAddr + size;

#ifdef CONFIG_ARM64
	__dma_flush_range((void *)range.start, range.end - range.start);
#else
	dmac_flush_range((void *)range.start, (void *)range.end);
#endif
}

struct disp_ion_mem *disp_ion_malloc(u32 num_bytes, void *phys_addr)
{
	struct disp_ion_mgr *ion_mgr = &(g_disp_drv.ion_mgr);
	struct ion_client *client = NULL;
	struct disp_ion_list_node *ion_node = NULL;
	struct disp_ion_mem *mem = NULL;
	u32 *paddr = NULL;
	int ret = -1;

	if (NULL == ion_mgr) {
		__wrn("disp ion manager has not initial yet\n");
		return NULL;
	}

	ion_node = kmalloc(sizeof(struct disp_ion_list_node), GFP_KERNEL);
	if (NULL == ion_node) {
		__wrn("fail to alloc ion node, size=%u\n",
		      (unsigned int)sizeof(struct disp_ion_list_node));
		return NULL;
	}

	mutex_lock(&(ion_mgr->mlock));
	client = ion_mgr->client;
	mem = &ion_node->mem;
	mem->size = MY_BYTE_ALIGN(num_bytes);
	ret = __disp_ion_alloc_coherent(client, mem);
	if (0 != ret) {
		__wrn("fail to alloc ion, ret=%d\n", ret);
		goto err_hdl;
	}

	paddr = (u32 *)phys_addr;
	*paddr = (u32)mem->p_item->dma_addr;
	list_add_tail(&(ion_node->node), &(ion_mgr->ion_list));

	mutex_unlock(&(ion_mgr->mlock));

	return mem;

err_hdl:
	if (ion_node)
		kfree(ion_node);
	mutex_unlock(&(ion_mgr->mlock));

	return NULL;
}

void disp_ion_free(void *virt_addr, void *phys_addr, u32 num_bytes)
{
	struct disp_ion_mgr *ion_mgr = &(g_disp_drv.ion_mgr);
	struct ion_client *client = NULL;
	struct disp_ion_list_node *ion_node = NULL, *tmp_ion_node = NULL;
	struct disp_ion_mem *mem = NULL;
	bool found = false;

	if (NULL == ion_mgr) {
		__wrn("disp ion manager has not initial yet\n");
		return;
	}

	client = ion_mgr->client;

	mutex_lock(&(ion_mgr->mlock));
	list_for_each_entry_safe(ion_node, tmp_ion_node, &ion_mgr->ion_list,
				 node) {
		if (NULL != ion_node) {
			mem = &ion_node->mem;
			if ((((unsigned long)mem->p_item->dma_addr) ==
			     ((unsigned long)phys_addr)) &&
			    (((unsigned long)mem->vaddr) ==
			     ((unsigned long)virt_addr))) {
				__disp_ion_free_coherent(client, mem);
				__list_del_entry(&(ion_node->node));
				found = true;
				break;
			}
		}
	}
	mutex_unlock(&(ion_mgr->mlock));

	if (false == found) {
		__wrn("vaddr=0x%p, paddr=0x%p is not found in ion\n", virt_addr,
		      phys_addr);
	}
}

static void deinit_disp_ion_mgr(struct disp_ion_mgr *ion_mgr)
{
	struct disp_ion_list_node *ion_node = NULL, *tmp_ion_node = NULL;
	struct disp_ion_mem *mem = NULL;
	struct ion_client *client = NULL;

	if (NULL == ion_mgr) {
		__wrn("input param is null\n");
		return;
	}

	client = ion_mgr->client;
	mutex_lock(&(ion_mgr->mlock));
	list_for_each_entry_safe(ion_node, tmp_ion_node, &ion_mgr->ion_list,
				 node) {
		if (NULL != ion_node) {
			// free all ion node
			mem = &ion_node->mem;
			__disp_ion_free_coherent(client, mem);
			__list_del_entry(&(ion_node->node));
			kfree(ion_node);
		}
	}
	ion_client_destroy(client);
	mutex_unlock(&(ion_mgr->mlock));
}
#endif

s32 disp_set_hdmi_func(struct disp_device_func *func)
{
	return bsp_disp_set_hdmi_func(func);
}
EXPORT_SYMBOL(disp_set_hdmi_func);

s32 disp_set_edp_func(struct disp_tv_func *func)
{
	return bsp_disp_set_edp_func(func);
}
EXPORT_SYMBOL(disp_set_edp_func);

s32 disp_set_vdpo_func(struct disp_tv_func *func)
{
	return bsp_disp_set_vdpo_func(func);
}
EXPORT_SYMBOL(disp_set_vdpo_func);

s32 disp_set_hdmi_detect(bool hpd)
{
	return bsp_disp_hdmi_set_detect(hpd);
}
EXPORT_SYMBOL(disp_set_hdmi_detect);

s32 disp_tv_register(struct disp_tv_func *func)
{
	return bsp_disp_tv_register(func);
}
EXPORT_SYMBOL(disp_tv_register);

static void resume_proc(unsigned disp, struct disp_manager *mgr)
{
	if (!mgr || !mgr->device)
		return;

	if (mgr->device->type == DISP_OUTPUT_TYPE_LCD)
		mgr->device->fake_enable(mgr->device);
}

static void resume_work_0(struct work_struct *work)
{
	resume_proc(0, g_disp_drv.mgr[0]);
}

#if DISP_SCREEN_NUM > 1
static void resume_work_1(struct work_struct *work)
{
	resume_proc(1, g_disp_drv.mgr[1]);
}
#endif

int disp_device_set_config(struct disp_init_para *init,
					unsigned int screen_id)
{
	struct disp_device_config config;

	memset(&config, 0, sizeof(struct disp_device_config));
	config.type = init->output_type[screen_id];
	config.mode = init->output_mode[screen_id];
	config.format = init->output_format[screen_id];
	config.bits = init->output_bits[screen_id];
	config.eotf = init->output_eotf[screen_id];
	config.cs = init->output_cs[screen_id];
	config.dvi_hdmi = init->output_dvi_hdmi[screen_id];
	config.range = init->output_range[screen_id];
	config.scan = init->output_scan[screen_id];
	config.aspect_ratio = init->output_aspect_ratio[screen_id];
	if (!init->using_device_config)
		return bsp_disp_device_switch(screen_id,
					      config.type,
					      config.mode);
	else
		return bsp_disp_device_set_config(screen_id, &config);
}

static void start_work(struct work_struct *work)
{
	int num_screens;
	int screen_id;
	int count = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	while ((g_disp_drv.inited == 0) && (count < 5)) {
		count++;
		msleep(20);
	}
	if (count >= 5)
		pr_warn("%s, timeout\n", __func__);
	if (g_disp_drv.para.boot_info.sync == 0) {
		for (screen_id = 0; screen_id < num_screens; screen_id++) {
			int disp_mode = g_disp_drv.disp_init.disp_mode;
			int output_type =
			    g_disp_drv.disp_init.output_type[screen_id];
			int output_mode =
			    g_disp_drv.disp_init.output_mode[screen_id];
			int lcd_registered =
			    bsp_disp_get_lcd_registered(screen_id);
			int hdmi_registered = bsp_disp_get_hdmi_registered();

			__inf
			    ("sel=%d, output_type=%d, lcd_reg=%d,hdmi_reg=%d\n",
			     screen_id, output_type, lcd_registered,
			     hdmi_registered);
			if (((disp_mode == DISP_INIT_MODE_SCREEN0)
			     && (screen_id == 0))
			    || ((disp_mode == DISP_INIT_MODE_SCREEN1)
				&& (screen_id == 1))) {
				if (output_type == DISP_OUTPUT_TYPE_LCD) {
					if (lcd_registered &&
					    bsp_disp_get_output_type(screen_id)
					    != DISP_OUTPUT_TYPE_LCD) {
						bsp_disp_device_switch(screen_id, output_type, output_mode);
						suspend_output_type[screen_id] =
						    output_type;
					}
				} else if (output_type
				    == DISP_OUTPUT_TYPE_HDMI) {
					if (hdmi_registered &&
					    bsp_disp_get_output_type(screen_id)
					    != DISP_OUTPUT_TYPE_HDMI) {
						msleep(600);
						bsp_disp_device_switch(screen_id, output_type, output_mode);
						suspend_output_type[screen_id] =
						    output_type;
					}
				} else {
					bsp_disp_device_switch(screen_id,
							       output_type,
							       output_mode);
					suspend_output_type[screen_id] =
					    output_type;
				}
			}
		}
	} else {
		if ((g_disp_drv.para.boot_info.type == DISP_OUTPUT_TYPE_HDMI)
		    && !bsp_disp_get_hdmi_registered())
			return;
		if (bsp_disp_get_output_type(g_disp_drv.para.boot_info.disp) !=
		    g_disp_drv.para.boot_info.type) {
			bsp_disp_sync_with_hw(&g_disp_drv.para);
			suspend_output_type[g_disp_drv.para.boot_info.disp] =
			    g_disp_drv.para.boot_info.type;
		}
	}
}

static s32 start_process(void)
{
	flush_work(&g_disp_drv.start_work);
#if !defined(CONFIG_EINK_PANEL_USED)
	schedule_work(&g_disp_drv.start_work);
#endif
	return 0;
}

s32 disp_register_sync_proc(void (*proc) (u32))
{
	struct proc_list *new_proc;

	new_proc =
	    (struct proc_list *)disp_sys_malloc(sizeof(struct proc_list));
	if (new_proc) {
		new_proc->proc = proc;
		list_add_tail(&(new_proc->list),
			      &(g_disp_drv.sync_proc_list.list));
	} else {
		pr_warn("malloc fail in %s\n", __func__);
	}

	return 0;
}

s32 disp_unregister_sync_proc(void (*proc) (u32))
{
	struct proc_list *ptr, *ptrtmp;

	if (proc == NULL) {
		pr_warn("hdl is NULL in %s\n", __func__);
		return -1;
	}
	list_for_each_entry_safe(ptr,
				 ptrtmp,
				 &g_disp_drv.sync_proc_list.list,
				 list) {
		if (ptr->proc == proc) {
			list_del(&ptr->list);
			kfree((void *)ptr);
			return 0;
		}
	}

	return -1;
}

s32 disp_register_sync_finish_proc(void (*proc) (u32))
{
	struct proc_list *new_proc;

	new_proc =
	    (struct proc_list *)disp_sys_malloc(sizeof(struct proc_list));
	if (new_proc) {
		new_proc->proc = proc;
		list_add_tail(&(new_proc->list),
			      &(g_disp_drv.sync_finish_proc_list.list));
	} else {
		pr_warn("malloc fail in %s\n", __func__);
	}

	return 0;
}

s32 disp_unregister_sync_finish_proc(void (*proc) (u32))
{
	struct proc_list *ptr, *ptrtmp;
	unsigned long flags;

	spin_lock_irqsave(&sync_finish_lock, flags);
	if (proc == NULL) {
		pr_warn("hdl is NULL in %s\n", __func__);
		return -1;
	}
	list_for_each_entry_safe(ptr,
				 ptrtmp,
				 &g_disp_drv.sync_finish_proc_list.list,
				 list) {
		if (ptr->proc == proc) {
			list_del(&ptr->list);
			kfree((void *)ptr);
			return 0;
		}
	}
	spin_unlock_irqrestore(&sync_finish_lock, flags);

	return -1;
}

static s32 disp_sync_finish_process(u32 screen_id)
{
	struct proc_list *ptr;
	unsigned long flags;

	spin_lock_irqsave(&sync_finish_lock, flags);
	list_for_each_entry(ptr, &g_disp_drv.sync_finish_proc_list.list, list) {
		if (ptr->proc)
			ptr->proc(screen_id);
	}
	spin_unlock_irqrestore(&sync_finish_lock, flags);

	return 0;
}

s32 disp_register_ioctl_func(unsigned int cmd,
			     int (*proc)(unsigned int cmd, unsigned long arg))
{
	struct ioctl_list *new_proc;

	new_proc =
	    (struct ioctl_list *)disp_sys_malloc(sizeof(struct ioctl_list));
	if (new_proc) {
		new_proc->cmd = cmd;
		new_proc->func = proc;
		list_add_tail(&(new_proc->list),
			      &(g_disp_drv.ioctl_extend_list.list));
	} else {
		pr_warn("malloc fail in %s\n", __func__);
	}

	return 0;
}

s32 disp_unregister_ioctl_func(unsigned int cmd)
{
	struct ioctl_list *ptr;

	list_for_each_entry(ptr, &g_disp_drv.ioctl_extend_list.list, list) {
		if (ptr->cmd == cmd) {
			list_del(&ptr->list);
			kfree((void *)ptr);
			return 0;
		}
	}

	pr_warn("no ioctl found(cmd:0x%x) in %s\n", cmd, __func__);
	return -1;
}

static s32 disp_ioctl_extend(unsigned int cmd, unsigned long arg)
{
	struct ioctl_list *ptr;

	list_for_each_entry(ptr, &g_disp_drv.ioctl_extend_list.list, list) {
		if (cmd == ptr->cmd)
			return ptr->func(cmd, arg);
	}

	return -1;
}

s32 disp_register_compat_ioctl_func(unsigned int cmd,
				    int (*proc)(unsigned int cmd,
						 unsigned long arg))
{
	struct ioctl_list *new_proc;

	new_proc =
	    (struct ioctl_list *)disp_sys_malloc(sizeof(struct ioctl_list));
	if (new_proc) {
		new_proc->cmd = cmd;
		new_proc->func = proc;
		list_add_tail(&(new_proc->list),
			      &(g_disp_drv.compat_ioctl_extend_list.list));
	} else {
		pr_warn("malloc fail in %s\n", __func__);
	}

	return 0;
}

s32 disp_unregister_compat_ioctl_func(unsigned int cmd)
{
	struct ioctl_list *ptr;

	list_for_each_entry(ptr, &g_disp_drv.compat_ioctl_extend_list.list,
			    list) {
		if (ptr->cmd == cmd) {
			list_del(&ptr->list);
			kfree((void *)ptr);
			return 0;
		}
	}

	pr_warn("no ioctl found(cmd:0x%x) in %s\n", cmd, __func__);
	return -1;
}

#ifdef CONFIG_COMPAT
static s32 disp_compat_ioctl_extend(unsigned int cmd, unsigned long arg)
{
	struct ioctl_list *ptr;

	list_for_each_entry(ptr, &g_disp_drv.compat_ioctl_extend_list.list,
			    list) {
		if (cmd == ptr->cmd)
			return ptr->func(cmd, arg);
	}

	return -1;
}
#endif

s32 disp_register_standby_func(int (*suspend) (void), int (*resume) (void))
{
	struct standby_cb_list *new_proc;

	new_proc = (struct standby_cb_list *)disp_sys_malloc(
	    sizeof(struct standby_cb_list));
	if (new_proc) {
		new_proc->suspend = suspend;
		new_proc->resume = resume;
		list_add_tail(&(new_proc->list),
			      &(g_disp_drv.stb_cb_list.list));
	} else {
		pr_warn("malloc fail in %s\n", __func__);
	}

	return 0;
}

s32 disp_unregister_standby_func(int (*suspend) (void), int (*resume) (void))
{
	struct standby_cb_list *ptr;

	list_for_each_entry(ptr, &g_disp_drv.stb_cb_list.list, list) {
		if ((ptr->suspend == suspend) && (ptr->resume == resume)) {
			list_del(&ptr->list);
			kfree((void *)ptr);
			return 0;
		}
	}

	return -1;
}

static s32 disp_suspend_cb(void)
{
	struct standby_cb_list *ptr;

	list_for_each_entry(ptr, &g_disp_drv.stb_cb_list.list, list) {
		if (ptr->suspend)
			return ptr->suspend();
	}

	return -1;
}

static s32 disp_resume_cb(void)
{
	struct standby_cb_list *ptr;

	list_for_each_entry(ptr, &g_disp_drv.stb_cb_list.list, list) {
		if (ptr->resume)
			return ptr->resume();
	}

	return -1;
}

static s32 disp_init(struct platform_device *pdev)
{
	struct disp_bsp_init_para *para;
	int i, disp, num_screens;
	unsigned int value, value1, value2, output_type, output_mode;
	unsigned int output_format, output_bits, output_eotf, output_cs;

	__inf("%s !\n", __func__);

	INIT_WORK(&g_disp_drv.resume_work[0], resume_work_0);
#if DISP_SCREEN_NUM > 1
	INIT_WORK(&g_disp_drv.resume_work[1], resume_work_1);
#endif
	/* INIT_WORK(&g_disp_drv.resume_work[2], resume_work_2); */
	INIT_WORK(&g_disp_drv.start_work, start_work);
	INIT_LIST_HEAD(&g_disp_drv.sync_proc_list.list);
	INIT_LIST_HEAD(&g_disp_drv.sync_finish_proc_list.list);
	INIT_LIST_HEAD(&g_disp_drv.ioctl_extend_list.list);
	INIT_LIST_HEAD(&g_disp_drv.compat_ioctl_extend_list.list);
	INIT_LIST_HEAD(&g_disp_drv.stb_cb_list.list);
	mutex_init(&g_disp_drv.mlock);
	spin_lock_init(&sync_finish_lock);
	parser_disp_init_para(pdev->dev.of_node, &g_disp_drv.disp_init);
	para = &g_disp_drv.para;

	memset(para, 0, sizeof(struct disp_bsp_init_para));
	for (i = 0; i < DISP_MOD_NUM; i++) {
		para->reg_base[i] = g_disp_drv.reg_base[i];
		para->irq_no[i] = g_disp_drv.irq_no[i];
		para->mclk[i] = g_disp_drv.mclk[i];
		__inf("mod %d, base=0x%lx, irq=%d, mclk=0x%p\n", i,
		      para->reg_base[i], para->irq_no[i], para->mclk[i]);
	}

	para->disp_int_process = disp_sync_finish_process;
	para->vsync_event = drv_disp_vsync_event;
	para->start_process = start_process;

	value = disp_boot_para_parse("boot_disp");
	value1 = disp_boot_para_parse("boot_disp1");
	value2 = disp_boot_para_parse("boot_disp2");
	output_type = (value >> 8) & 0xff;
	output_mode = (value) & 0xff;

	output_format = (value1 >> 0) & 0xff;
	output_bits = (value1 >> 8) & 0xff;
	output_cs = (value1 >> 16) & 0xffff;
	output_eotf = (value2 >> 0) & 0xff;

	if (output_type != (int)DISP_OUTPUT_TYPE_NONE) {
		para->boot_info.sync = 1;
		para->boot_info.disp = 0;	/* disp0 */
		para->boot_info.type = output_type;
		para->boot_info.mode = output_mode;
		para->boot_info.format = output_format;
		para->boot_info.bits = output_bits;
		para->boot_info.cs = output_cs;
		para->boot_info.eotf = output_eotf;
	} else {
		output_type = (value >> 24) & 0xff;
		output_mode = (value >> 16) & 0xff;
		if (output_type != (int)DISP_OUTPUT_TYPE_NONE) {
			para->boot_info.sync = 1;
			para->boot_info.disp = 1;	/* disp1 */
			para->boot_info.type = output_type;
			para->boot_info.mode = output_mode;
			para->boot_info.format = output_format;
			para->boot_info.bits = output_bits;
			para->boot_info.cs = output_cs;
			para->boot_info.eotf = output_eotf;
		}
	}

	para->boot_info.dvi_hdmi =
		g_disp_drv.disp_init.output_dvi_hdmi[para->boot_info.disp];
	para->boot_info.range =
		g_disp_drv.disp_init.output_range[para->boot_info.disp];
	para->boot_info.scan =
		g_disp_drv.disp_init.output_scan[para->boot_info.disp];
	para->boot_info.aspect_ratio =
		g_disp_drv.disp_init.output_aspect_ratio[para->boot_info.disp];

	if (para->boot_info.sync == 1) {
		__wrn("smooth display screen:%d type:%d", para->boot_info.disp,
		      para->boot_info.type);
		g_disp_drv.disp_init.disp_mode = para->boot_info.disp;
		g_disp_drv.disp_init.output_type[para->boot_info.disp] =
		    output_type;
		g_disp_drv.disp_init.output_mode[para->boot_info.disp] =
								output_mode;
		g_disp_drv.disp_init.output_format[para->boot_info.disp] =
								output_format;
		g_disp_drv.disp_init.output_bits[para->boot_info.disp] =
								output_bits;
		g_disp_drv.disp_init.output_cs[para->boot_info.disp] =
								output_cs;
		g_disp_drv.disp_init.output_eotf[para->boot_info.disp] =
								output_eotf;
	} else {
#if defined(CONFIG_SUNXI_IOMMU)
		sunxi_enable_device_iommu(DE_MASTOR_ID, true);
#endif
	}

	bsp_disp_init(para);

	/*if (bsp_disp_check_device_enabled(para) == 0)
		para->boot_info.sync = 0;
	*/
	num_screens = bsp_disp_feat_get_num_screens();
	for (disp = 0; disp < num_screens; disp++)
		g_disp_drv.mgr[disp] = disp_get_layer_manager(disp);
#if defined(SUPPORT_EINK)
	g_disp_drv.eink_manager[0] = disp_get_eink_manager(0);
#endif
	lcd_init();
	bsp_disp_open();

	fb_init(pdev);
#if defined(CONFIG_DISP2_SUNXI_COMPOSER)
	composer_init(&g_disp_drv);
#endif
	g_disp_drv.inited = true;
	start_process();

	__inf("%s finish\n", __func__);
	return 0;
}

static s32 disp_exit(void)
{
	fb_exit();
	bsp_disp_close();
	bsp_disp_exit(g_disp_drv.exit_mode);
	return 0;
}

static int disp_mem_request(int sel, u32 size)
{
#ifndef FB_RESERVED_MEM
	unsigned map_size = 0;
	struct page *page;

	if ((sel >= DISP_MEM_NUM) ||
	    (g_disp_mm[sel].info_base != NULL)) {
		__wrn("invalid param\n");
		return -EINVAL;
	}

	g_disp_mm[sel].mem_len = size;
	map_size = PAGE_ALIGN(g_disp_mm[sel].mem_len);

	page = alloc_pages(GFP_KERNEL, get_order(map_size));
	if (page != NULL) {
		g_disp_mm[sel].info_base = page_address(page);
		if (g_disp_mm[sel].info_base == NULL) {
			free_pages((unsigned long)(page), get_order(map_size));
			__wrn("page_address fail!\n");
			return -ENOMEM;
		}
		g_disp_mm[sel].mem_start =
		    virt_to_phys(g_disp_mm[sel].info_base);
		memset(g_disp_mm[sel].info_base, 0, size);

		__inf("pa=0x%p va=0x%p size:0x%x\n",
		      (void *)g_disp_mm[sel].mem_start,
		      g_disp_mm[sel].info_base, size);
		return 0;
	}

	__wrn("alloc_pages fail!\n");
	return -ENOMEM;
#else
	uintptr_t phy_addr;

	if ((sel >= DISP_MEM_NUM) ||
	    (g_disp_mm[sel].info_base != NULL)) {
		__wrn("invalid param\n");
		return -EINVAL;
	}

	g_disp_mm[sel].info_base = disp_malloc(size, (void *)&phy_addr);
	if (g_disp_mm[sel].info_base) {
		g_disp_mm[sel].mem_start = phy_addr;
		g_disp_mm[sel].mem_len = size;
		memset(g_disp_mm[sel].info_base, 0, size);
		__inf("pa=0x%p va=0x%p size:0x%x\n",
		      (void *)g_disp_mm[sel].mem_start,
		      g_disp_mm[sel].info_base, size);

		return 0;
	}

	__wrn("disp_malloc fail!\n");
	return -ENOMEM;
#endif
}

static int disp_mem_release(int sel)
{
#ifndef FB_RESERVED_MEM
	unsigned map_size;
	unsigned page_size;

	if (g_disp_mm[sel].info_base == NULL) {
		__wrn("invalid param\n");
		return -EINVAL;
	}

	map_size = PAGE_ALIGN(g_disp_mm[sel].mem_len);
	page_size = map_size;

	free_pages((unsigned long)(g_disp_mm[sel].info_base),
		   get_order(page_size));
	memset(&g_disp_mm[sel], 0, sizeof(struct info_mm));
#else
	if (g_disp_mm[sel].info_base == NULL)
		return -EINVAL;

	__inf("disp_mem_release, mem_id=%d, phy_addr=0x%p\n", sel,
	      (void *)g_disp_mm[sel].mem_start);
	disp_free((void *)g_disp_mm[sel].info_base,
		  (void *)g_disp_mm[sel].mem_start, g_disp_mm[sel].mem_len);
	memset(&g_disp_mm[sel], 0, sizeof(struct info_mm));
#endif
	return 0;
}

static bool disp_mem_check_valid(unsigned long pgoff)
{
	unsigned int i;
	bool ret = false;
	unsigned long mem_start = pgoff << PAGE_SHIFT;

	for (i = 0; i < DISP_MEM_NUM; i++) {
		if ((g_disp_mm[i].info_base != NULL) &&
		    (g_disp_mm[i].mem_start == mem_start)) {
			ret = true;
			break;
		}
	}

	return ret;
}

int sunxi_disp_get_source_ops(struct sunxi_disp_source_ops *src_ops)
{
	memset((void *)src_ops, 0, sizeof(struct sunxi_disp_source_ops));

	src_ops->sunxi_lcd_set_panel_funs = bsp_disp_lcd_set_panel_funs;
	src_ops->sunxi_lcd_delay_ms = disp_delay_ms;
	src_ops->sunxi_lcd_delay_us = disp_delay_us;
	src_ops->sunxi_lcd_backlight_enable = bsp_disp_lcd_backlight_enable;
	src_ops->sunxi_lcd_backlight_disable = bsp_disp_lcd_backlight_disable;
	src_ops->sunxi_lcd_pwm_enable = bsp_disp_lcd_pwm_enable;
	src_ops->sunxi_lcd_pwm_disable = bsp_disp_lcd_pwm_disable;
	src_ops->sunxi_lcd_power_enable = bsp_disp_lcd_power_enable;
	src_ops->sunxi_lcd_power_disable = bsp_disp_lcd_power_disable;
	src_ops->sunxi_lcd_tcon_enable = bsp_disp_lcd_tcon_enable;
	src_ops->sunxi_lcd_tcon_disable = bsp_disp_lcd_tcon_disable;
	src_ops->sunxi_lcd_pin_cfg = bsp_disp_lcd_pin_cfg;
	src_ops->sunxi_lcd_gpio_set_value = bsp_disp_lcd_gpio_set_value;
	src_ops->sunxi_lcd_gpio_set_direction = bsp_disp_lcd_gpio_set_direction;
#ifdef SUPPORT_DSI
	src_ops->sunxi_lcd_dsi_dcs_write = bsp_disp_lcd_dsi_dcs_wr;
	src_ops->sunxi_lcd_dsi_gen_write = bsp_disp_lcd_dsi_gen_wr;
	src_ops->sunxi_lcd_dsi_clk_enable = bsp_disp_lcd_dsi_clk_enable;
	src_ops->sunxi_lcd_dsi_mode_switch = bsp_disp_lcd_dsi_mode_switch;
	src_ops->sunxi_lcd_dsi_gen_short_read = bsp_disp_lcd_dsi_gen_short_read;
	src_ops->sunxi_lcd_dsi_dcs_read = bsp_disp_lcd_dsi_dcs_read;
	src_ops->sunxi_lcd_dsi_set_max_ret_size = bsp_disp_lcd_set_max_ret_size;
#endif
	src_ops->sunxi_lcd_cpu_write = tcon0_cpu_wr_16b;
	src_ops->sunxi_lcd_cpu_write_data = tcon0_cpu_wr_16b_data;
	src_ops->sunxi_lcd_cpu_write_index = tcon0_cpu_wr_16b_index;
	src_ops->sunxi_lcd_cpu_set_auto_mode = tcon0_cpu_set_auto_mode;

	return 0;
}

int disp_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long mypfn = vma->vm_pgoff;
	unsigned long vmsize = vma->vm_end - vma->vm_start;

	if (!disp_mem_check_valid(mypfn))
		return -EINVAL;

	vma->vm_pgoff = 0;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	if (remap_pfn_range
	    (vma, vma->vm_start, mypfn, vmsize, vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

int disp_open(struct inode *inode, struct file *file)
{
	return 0;
}

int disp_release(struct inode *inode, struct file *file)
{
	return 0;
}

ssize_t disp_read(struct file *file, char __user *buf, size_t count,
		  loff_t *ppos)
{
	return 0;
}

ssize_t disp_write(struct file *file, const char __user *buf, size_t count,
		   loff_t *ppos)
{
	return 0;
}

static u64 disp_dmamask = DMA_BIT_MASK(32);
static int disp_probe(struct platform_device *pdev)
{
	int i;
	int ret;
	int counter = 0;

	__inf("[DISP]disp_probe\n");
	memset(&g_disp_drv, 0, sizeof(struct disp_drv_info));

	g_disp_drv.dev = &pdev->dev;
	pdev->dev.dma_mask = &disp_dmamask;

	/* iomap */
	/* de - [device(tcon-top)] - lcd0/1/2.. - dsi */
	counter = 0;
	g_disp_drv.reg_base[DISP_MOD_DE] =
	    (uintptr_t __force) of_iomap(pdev->dev.of_node, counter);
	if (!g_disp_drv.reg_base[DISP_MOD_DE]) {
		dev_err(&pdev->dev, "unable to map de registers\n");
		ret = -EINVAL;
		goto err_iomap;
	}
	counter++;

#if defined(HAVE_DEVICE_COMMON_MODULE)
	g_disp_drv.reg_base[DISP_MOD_DEVICE] =
	    (uintptr_t __force) of_iomap(pdev->dev.of_node, counter);
	if (!g_disp_drv.reg_base[DISP_MOD_DEVICE]) {
		dev_err(&pdev->dev,
			"unable to map device common module registers\n");
		ret = -EINVAL;
		goto err_iomap;
	}
	counter++;
#endif

	for (i = 0; i < DISP_DEVICE_NUM; i++) {
		g_disp_drv.reg_base[DISP_MOD_LCD0 + i] =
		    (uintptr_t __force) of_iomap(pdev->dev.of_node, counter);
		if (!g_disp_drv.reg_base[DISP_MOD_LCD0 + i]) {
			dev_err(&pdev->dev,
				"unable to map timing controller %d registers\n",
				i);
			ret = -EINVAL;
			goto err_iomap;
		}
		counter++;
	}

#if defined(SUPPORT_DSI)
	for (i = 0; i < DEVICE_DSI_NUM; ++i) {
		g_disp_drv.reg_base[DISP_MOD_DSI0 + i] = (uintptr_t __force)
			of_iomap(pdev->dev.of_node, counter);
		if (!g_disp_drv.reg_base[DISP_MOD_DSI0 + i]) {
			dev_err(&pdev->dev, "unable to map dsi registers\n");
			ret = -EINVAL;
			goto err_iomap;
		}
		counter++;
	}
#endif

#if defined(SUPPORT_EINK)
	g_disp_drv.reg_base[DISP_MOD_EINK] =
		   (uintptr_t __force)of_iomap(pdev->dev.of_node, counter);
	if (!g_disp_drv.reg_base[DISP_MOD_EINK]) {
		dev_err(&pdev->dev, "unable to map eink registers\n");
		ret = -EINVAL;
		goto err_iomap;
	}
	counter++;
#endif

	/* parse and map irq */
	/* lcd0/1/2.. - dsi */
	counter = 0;
	for (i = 0; i < DISP_DEVICE_NUM; i++) {
		g_disp_drv.irq_no[DISP_MOD_LCD0 + i] =
		    irq_of_parse_and_map(pdev->dev.of_node, counter);
		if (!g_disp_drv.irq_no[DISP_MOD_LCD0 + i])
			dev_err(&pdev->dev,
				"get irq %d fail for timing controller%d\n",
				counter, i);

		counter++;
	}
#if defined(SUPPORT_DSI)
	for (i = 0; i < DEVICE_DSI_NUM; ++i) {
		g_disp_drv.irq_no[DISP_MOD_DSI0 + i] = irq_of_parse_and_map(
						 pdev->dev.of_node, counter);
		if (!g_disp_drv.irq_no[DISP_MOD_DSI0 + i])
			dev_err(&pdev->dev,
				"irq_of_parse_and_map irq %d fail for dsi\n",
				i);
		counter++;
	}
#endif

#if defined(SUPPORT_VDPO)
	g_disp_drv.irq_no[DISP_MOD_VDPO] =
	    irq_of_parse_and_map(pdev->dev.of_node, counter);
	if (!g_disp_drv.irq_no[DISP_MOD_DSI0])
		dev_err(&pdev->dev,
			"irq_of_parse_and_map irq fail for vdpo\n");
	++counter;
#endif /*endif SUPPORT_VDPO */

#if defined(SUPPORT_EINK)
	g_disp_drv.irq_no[DISP_MOD_DE] =
	    irq_of_parse_and_map(pdev->dev.of_node, counter);
	if (!g_disp_drv.irq_no[DISP_MOD_DE])
		dev_err(&pdev->dev,
			"irq_of_parse_and_map de irq %d fail for de\n", i);
	counter++;

	g_disp_drv.irq_no[DISP_MOD_EINK] =
	    irq_of_parse_and_map(pdev->dev.of_node, counter);
	if (!g_disp_drv.irq_no[DISP_MOD_EINK])
		dev_err(&pdev->dev,
			"irq_of_parse_and_map eink irq %d fail for ee\n", i);
	counter++;
#endif

	/* get clk */
	/* de - [device(tcon-top)] - lcd0/1/2.. - lvds - dsi */
	counter = 0;
	g_disp_drv.mclk[DISP_MOD_DE] = of_clk_get(pdev->dev.of_node, counter);
	if (IS_ERR(g_disp_drv.mclk[DISP_MOD_DE]))
		dev_err(&pdev->dev, "fail to get clk for de\n");
	counter++;

#if defined(HAVE_DEVICE_COMMON_MODULE)
	g_disp_drv.mclk[DISP_MOD_DEVICE] =
	    of_clk_get(pdev->dev.of_node, counter);
	if (IS_ERR(g_disp_drv.mclk[DISP_MOD_DEVICE]))
		dev_err(&pdev->dev,
			"fail to get clk for device common module\n");
	counter++;
#endif

	for (i = 0; i < DISP_DEVICE_NUM; i++) {
		g_disp_drv.mclk[DISP_MOD_LCD0 + i] =
		    of_clk_get(pdev->dev.of_node, counter);
		if (IS_ERR(g_disp_drv.mclk[DISP_MOD_LCD0 + i]))
			dev_err(&pdev->dev,
				"fail to get clk for timing controller%d\n", i);
		counter++;
	}

#if defined(SUPPORT_LVDS)
	g_disp_drv.mclk[DISP_MOD_LVDS] = of_clk_get(pdev->dev.of_node, counter);
	if (IS_ERR(g_disp_drv.mclk[DISP_MOD_LVDS]))
		dev_err(&pdev->dev, "fail to get clk for lvds\n");
	counter++;
#endif

#if defined(SUPPORT_DSI)
	for (i = 0; i < CLK_DSI_NUM; ++i) {
		g_disp_drv.mclk[DISP_MOD_DSI0 + i] = of_clk_get(
				       pdev->dev.of_node, counter);
		if (IS_ERR(g_disp_drv.mclk[DISP_MOD_DSI0 + i]))
			dev_err(&pdev->dev, "fail to get clk %d for dsi\n", i);
		counter++;
	}

#endif

#if defined(SUPPORT_EINK)
	g_disp_drv.mclk[DISP_MOD_EINK] = of_clk_get(pdev->dev.of_node, counter);
	if (IS_ERR(g_disp_drv.mclk[DISP_MOD_EINK]))
		dev_err(&pdev->dev, "fail to get clk for eink\n");
	counter++;

	g_disp_drv.mclk[DISP_MOD_EDMA] = of_clk_get(pdev->dev.of_node, counter);
	if (IS_ERR(g_disp_drv.mclk[DISP_MOD_EDMA]))
		dev_err(&pdev->dev, "fail to get clk for edma\n");
	counter++;
#endif

#if defined(CONFIG_ION_SUNXI)
	init_disp_ion_mgr(&g_disp_drv.ion_mgr);
#endif

	disp_init(pdev);
	ret = sysfs_create_group(&display_dev->kobj, &disp_attribute_group);
	if (ret)
		__wrn("sysfs_create_group fail!\n");

	power_status_init = 1;
#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);
	/*pm_runtime_set_autosuspend_delay(&pdev->dev, 5000);*/
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
#endif
	device_enable_async_suspend(&pdev->dev);

	__inf("[DISP]disp_probe finish\n");

	return ret;

err_iomap:
	for (i = 0; i < DISP_DEVICE_NUM; i++) {
		if (g_disp_drv.reg_base[i])
			iounmap((char __iomem *)g_disp_drv.reg_base[i]);
	}

	return ret;
}

static int disp_remove(struct platform_device *pdev)
{
	int i;

	pr_info("disp_remove call\n");

	disp_shutdown(pdev);
#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_set_suspended(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
#endif
	disp_exit();
#if defined(CONFIG_ION_SUNXI)
	deinit_disp_ion_mgr(&g_disp_drv.ion_mgr);
#endif
	sysfs_remove_group(&display_dev->kobj, &disp_attribute_group);
	for (i = 0; i < DISP_MOD_NUM; i++) {
		if (g_disp_drv.mclk[i] && !IS_ERR(g_disp_drv.mclk[i]))
			clk_put(g_disp_drv.mclk[i]);
	}
	for (i = 0; i < DISP_MOD_NUM; i++) {
		if (g_disp_drv.reg_base[i])
			iounmap((char __iomem *)g_disp_drv.reg_base[i]);
	}
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static int disp_blank(bool blank)
{
	u32 screen_id = 0;
	int num_screens;
	struct disp_manager *mgr = NULL;

#if defined(CONFIG_DEVFREQ_DRAM_FREQ_WITH_SOFT_NOTIFY)
	/* notify dramfreq module that DE will access DRAM in a short time */
	if (!blank)
		dramfreq_master_access(MASTER_DE, true);
#endif
	num_screens = bsp_disp_feat_get_num_screens();

	for (screen_id = 0; screen_id < num_screens; screen_id++) {
		mgr = g_disp_drv.mgr[screen_id];
		/* Currently remove !mgr->device condition,
		 * avoiding problem in the following case:
		 *
		 *   attach manager and device -> disp blank --> blank success
		 *   deattach manager and device -> disp unblank --> fail
		 *   (cause don't satisfy !mgr->device condition)
		 *   attach manager and device --> problem arises
		 *   (manager will be always on unblank state)
		 *
		 * The scenario is: hdmi plug in -> enter standy
		 *  -> hdmi plug out -> exit standby -> hdmi plug in
		 *  -> display blank on hdmi screen
		 */
		if (!mgr)
			continue;

		if (mgr->blank)
			mgr->blank(mgr, blank);
	}

#if defined(CONFIG_DEVFREQ_DRAM_FREQ_WITH_SOFT_NOTIFY)
	/* notify dramfreq module that DE will not access DRAM any more */
	if (blank)
		dramfreq_master_access(MASTER_DE, false);
#endif

	return 0;
}

#if defined(CONFIG_PM_RUNTIME)
static int disp_runtime_suspend(struct device *dev)
{
	u32 screen_id = 0;
	int num_screens;
	struct disp_manager *mgr = NULL;
	struct disp_device *dispdev_suspend = NULL;
	struct list_head *disp_list = NULL;

	pr_info("%s\n", __func__);

	if (!g_pm_runtime_enable)
		return 0;

	num_screens = bsp_disp_feat_get_num_screens();

	disp_suspend_cb();
	for (screen_id = 0; screen_id < num_screens; screen_id++) {
		mgr = g_disp_drv.mgr[screen_id];
		if (mgr && mgr->device) {
			struct disp_device *dispdev = mgr->device;

			if (suspend_output_type[screen_id] ==
			    DISP_OUTPUT_TYPE_LCD)
				flush_work(&g_disp_drv.resume_work[screen_id]);

			if (dispdev->is_enabled(dispdev))
				dispdev->disable(dispdev);
		}
	}

	disp_list = disp_device_get_list_head();
	list_for_each_entry(dispdev_suspend, disp_list, list) {
		if (dispdev_suspend->suspend)
			dispdev_suspend->suspend(dispdev_suspend);
	}

	suspend_status |= DISPLAY_LIGHT_SLEEP;
	suspend_prestep = 0;

	pr_info("%s finish\n", __func__);

	return 0;
}

static int disp_runtime_resume(struct device *dev)
{
	u32 screen_id = 0;
	int num_screens;
	struct disp_manager *mgr = NULL;
	struct disp_device *dispdev = NULL;
	struct list_head *disp_list = NULL;
	struct disp_device_config config;

	pr_info("%s\n", __func__);

	if (!g_pm_runtime_enable)
		return 0;

	memset(&config, 0, sizeof(struct disp_device_config));
	num_screens = bsp_disp_feat_get_num_screens();

	disp_list = disp_device_get_list_head();
	list_for_each_entry(dispdev, disp_list, list) {
		if (dispdev->resume)
			dispdev->resume(dispdev);
	}

	for (screen_id = 0; screen_id < num_screens; screen_id++) {
		mgr = g_disp_drv.mgr[screen_id];
		if (!mgr || !mgr->device)
			continue;

		if (suspend_output_type[screen_id] == DISP_OUTPUT_TYPE_LCD) {
			flush_work(&g_disp_drv.resume_work[screen_id]);
			if (!mgr->device->is_enabled(mgr->device)) {
				mgr->device->enable(mgr->device);
			} else {
				mgr->device->pwm_enable(mgr->device);
				mgr->device->backlight_enable(mgr->device);
			}
		} else if (suspend_output_type[screen_id] !=
							DISP_OUTPUT_TYPE_NONE) {
			if (mgr->device->set_static_config &&
					mgr->device->get_static_config) {
				mgr->device->get_static_config(mgr->device,
								&config);

				mgr->device->set_static_config(mgr->device,
								&config);
			}
			if (!mgr->device->is_enabled(mgr->device))
				mgr->device->enable(mgr->device);
		}
	}

	suspend_status &= (~DISPLAY_LIGHT_SLEEP);
	suspend_prestep = 3;

	disp_resume_cb();

	pr_info("%s finish\n", __func__);

	return 0;
}

static int disp_runtime_idle(struct device *dev)
{
	u32 screen_id = 0;
	int num_screens;

	pr_info("%s\n", __func__);
	num_screens = bsp_disp_feat_get_num_screens();

	if (g_disp_drv.dev) {
		for (screen_id = 0; screen_id < num_screens; screen_id++) {
			if (suspend_output_type[screen_id] ==
			    DISP_OUTPUT_TYPE_LCD)
				pm_runtime_set_autosuspend_delay(g_disp_drv.dev,
								 5000);
		}
		pm_runtime_mark_last_busy(g_disp_drv.dev);
		pm_request_autosuspend(g_disp_drv.dev);
	} else {
		pr_warn("%s, display device is null\n", __func__);
	}

	/* return 0: for framework to request enter suspend.
	 * return non-zero: do susupend for myself;
	 */
	return -1;
}
#endif

int disp_suspend(struct device *dev)
{
	u32 screen_id = 0;
	int num_screens;
	struct disp_manager *mgr = NULL;
	struct disp_device *dispdev_suspend = NULL;
	struct list_head *disp_list = NULL;
	struct disp_device *dispdev = NULL;

#if defined(SUPPORT_EINK) && defined(CONFIG_EINK_PANEL_USED)
	struct disp_eink_manager *eink_manager = NULL;

	eink_manager = g_disp_drv.eink_manager[0];
	if (!eink_manager)
		__wrn("eink_manager is NULL!\n");
#endif
	pr_info("%s\n", __func__);

	if (!g_disp_drv.dev) {
		pr_warn("display device is null!\n");
		return 0;
	}
#if defined(CONFIG_PM_RUNTIME)
	if (!pm_runtime_status_suspended(g_disp_drv.dev))
#endif
	{
		num_screens = bsp_disp_feat_get_num_screens();
		disp_suspend_cb();
		if (g_pm_runtime_enable) {

			for (screen_id = 0; screen_id < num_screens;
			     screen_id++) {
				mgr = g_disp_drv.mgr[screen_id];
				if (!mgr || !mgr->device)
					continue;

				dispdev = mgr->device;
				if (suspend_output_type[screen_id] ==
				    DISP_OUTPUT_TYPE_LCD)
					flush_work(&g_disp_drv.
						   resume_work[screen_id]);
				if (suspend_output_type[screen_id] !=
				    DISP_OUTPUT_TYPE_NONE) {
					if (dispdev->is_enabled(dispdev))
						dispdev->disable(dispdev);
				}
			}
		} else {
			for (screen_id = 0; screen_id < num_screens;
			     screen_id++) {
				mgr = g_disp_drv.mgr[screen_id];
				if (!mgr || !mgr->device)
					continue;

				dispdev = mgr->device;
				if (suspend_output_type[screen_id] !=
				    DISP_OUTPUT_TYPE_NONE) {
					if (dispdev->is_enabled(dispdev))
						dispdev->disable(dispdev);
				}
			}
		}

		/*suspend for all display device */
		disp_list = disp_device_get_list_head();
		list_for_each_entry(dispdev_suspend, disp_list, list) {
			if (dispdev_suspend->suspend)
				dispdev_suspend->suspend(dispdev_suspend);
		}
	}
	/* FIXME: hdmi suspend */
	suspend_status |= DISPLAY_DEEP_SLEEP;
	suspend_prestep = 1;
#if defined(CONFIG_PM_RUNTIME)
	if (g_pm_runtime_enable) {
		pm_runtime_disable(g_disp_drv.dev);
		pm_runtime_set_suspended(g_disp_drv.dev);
		pm_runtime_enable(g_disp_drv.dev);
	}
#endif
	pr_info("%s finish\n", __func__);

#if defined(SUPPORT_EINK) && defined(CONFIG_EINK_PANEL_USED)
	eink_manager->suspend(eink_manager);
#endif
	return 0;
}

int disp_resume(struct device *dev)
{
	u32 screen_id = 0;
	int num_screens = bsp_disp_feat_get_num_screens();
	struct disp_manager *mgr = NULL;
	struct disp_device_config config;

#if defined(SUPPORT_EINK) && defined(CONFIG_EINK_PANEL_USED)
	struct disp_eink_manager *eink_manager = NULL;
#endif
#if defined(CONFIG_PM_RUNTIME)
	struct disp_device *dispdev = NULL;
	struct list_head *disp_list = NULL;

	if (dev->power.runtime_status == RPM_ACTIVE)
		goto late_resume;

	disp_list = disp_device_get_list_head();
	list_for_each_entry(dispdev, disp_list, list) {
		if (dispdev->resume)
			dispdev->resume(dispdev);
	}
	memset(&config, 0, sizeof(struct disp_device_config));
	if (g_pm_runtime_enable) {
		for (screen_id = 0; screen_id < num_screens; screen_id++) {
			mgr = g_disp_drv.mgr[screen_id];
			if (!mgr || !mgr->device)
				continue;

			if (suspend_output_type[screen_id] ==
			    DISP_OUTPUT_TYPE_LCD) {
				schedule_work(&g_disp_drv.
					      resume_work[screen_id]);
				if (!mgr->device->is_enabled(mgr->device)) {
					mgr->device->enable(mgr->device);
				} else {
					mgr->device->pwm_enable(mgr->device);
					mgr->device->backlight_enable(mgr->device);
				}
			} else if (suspend_output_type[screen_id] !=
						DISP_OUTPUT_TYPE_NONE) {
				if (mgr->device->set_static_config &&
					mgr->device->get_static_config) {
					mgr->device->get_static_config(mgr->device,
									&config);

					mgr->device->set_static_config(mgr->device,
									&config);
				}
				if (!mgr->device->is_enabled(mgr->device))
					mgr->device->enable(mgr->device);
			}
		}
		if (g_pm_runtime_enable) {
			if (g_disp_drv.dev) {
				pm_runtime_disable(g_disp_drv.dev);
				pm_runtime_set_active(g_disp_drv.dev);
				pm_runtime_enable(g_disp_drv.dev);
			} else {
				pr_warn("%s, display device is null\n",
					__func__);
			}
		}
	} else {
		for (screen_id = 0; screen_id < num_screens; screen_id++) {
			mgr = g_disp_drv.mgr[screen_id];
			if (!mgr || !mgr->device)
				continue;

			if (suspend_output_type[screen_id] !=
						DISP_OUTPUT_TYPE_NONE) {
				if (mgr->device->set_static_config
					&& mgr->device->get_static_config) {
					mgr->device->get_static_config(mgr->device, &config);
					mgr->device->set_static_config(mgr->device, &config);
				}
				if (!mgr->device->is_enabled(mgr->device))
					mgr->device->enable(mgr->device);
			}
		}
		disp_resume_cb();
	}
#else
	struct disp_device *dispdev = NULL;
	struct list_head *disp_list = NULL;

	memset(&config, 0, sizeof(struct disp_device_config));
	disp_list = disp_device_get_list_head();
	list_for_each_entry(dispdev, disp_list, list) {
		if (dispdev->resume)
			dispdev->resume(dispdev);
	}

	for (screen_id = 0; screen_id < num_screens; screen_id++) {
		mgr = g_disp_drv.mgr[screen_id];
		if (!mgr || !mgr->device)
			continue;

		if (suspend_output_type[screen_id] != DISP_OUTPUT_TYPE_NONE) {
			if (mgr->device->set_static_config &&
					mgr->device->get_static_config) {
				mgr->device->get_static_config(mgr->device,
								&config);

				mgr->device->set_static_config(mgr->device,
								&config);
			}
			mgr->device->enable(mgr->device);
		}
	}
	disp_resume_cb();
#endif

late_resume:
	suspend_status &= (~DISPLAY_DEEP_SLEEP);
	suspend_prestep = 2;

#if defined(SUPPORT_EINK) && defined(CONFIG_EINK_PANEL_USED)
	eink_manager = g_disp_drv.eink_manager[0];
	if (!eink_manager)
		__wrn("eink_manager is NULL!\n");
	eink_manager->resume(eink_manager);
#endif
	pr_info("%s finish\n", __func__);

	return 0;
}

static const struct dev_pm_ops disp_runtime_pm_ops = {
#ifdef CONFIG_PM_RUNTIME
	.runtime_suspend = disp_runtime_suspend,
	.runtime_resume = disp_runtime_resume,
	.runtime_idle = disp_runtime_idle,
#endif
	.suspend = disp_suspend,
	.resume = disp_resume,
};

static void disp_shutdown(struct platform_device *pdev)
{
	u32 screen_id = 0;
	int num_screens;

	num_screens = bsp_disp_feat_get_num_screens();

	for (screen_id = 0; screen_id < num_screens; screen_id++) {
		struct disp_manager *mgr = g_disp_drv.mgr[screen_id];

		if (mgr && mgr->device && mgr->device->is_enabled
		    && mgr->device->disable)
			if (mgr->device->is_enabled(mgr->device))
				mgr->device->disable(mgr->device);
	}
#if defined(CONFIG_SUNXI_IOMMU)
	sunxi_enable_device_iommu(DE_MASTOR_ID, false);
#endif
}

#ifdef EINK_FLUSH_TIME_TEST
struct timeval ioctrl_start_timer;
#endif
long disp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	unsigned long karg[4];
	unsigned long ubuffer[4] = { 0 };
	s32 ret = 0;
	int num_screens = 2;
	struct disp_manager *mgr = NULL;
	struct disp_device *dispdev = NULL;
	struct disp_enhance *enhance = NULL;
	struct disp_smbl *smbl = NULL;
	struct disp_capture *cptr = NULL;
#if defined(SUPPORT_EINK)
	struct disp_eink_manager *eink_manager = NULL;
#endif

#ifdef EINK_FLUSH_TIME_TEST
	do_gettimeofday(&ioctrl_start_timer);
#endif				/*test eink time */

	num_screens = bsp_disp_feat_get_num_screens();

	if (copy_from_user
	    ((void *)karg, (void __user *)arg, 4 * sizeof(unsigned long))) {
		__wrn("copy_from_user fail\n");
		return -EFAULT;
	}

	ubuffer[0] = *(unsigned long *)karg;
	ubuffer[1] = (*(unsigned long *)(karg + 1));
	ubuffer[2] = (*(unsigned long *)(karg + 2));
	ubuffer[3] = (*(unsigned long *)(karg + 3));

	if (ubuffer[0] < num_screens)
		mgr = g_disp_drv.mgr[ubuffer[0]];
	if (mgr) {
		dispdev = mgr->device;
		enhance = mgr->enhance;
		smbl = mgr->smbl;
		cptr = mgr->cptr;
	}
#if defined(SUPPORT_EINK)
	eink_manager = g_disp_drv.eink_manager[0];

	if (!eink_manager)
		__wrn("eink_manager is NULL!\n");

#endif

	if (cmd < DISP_FB_REQUEST) {
		if (ubuffer[0] >= num_screens) {
			__wrn
			    ("para err, cmd = 0x%x,screen id = %d\n",
			     cmd, (int)ubuffer[0]);
			return -1;
		}
	}
	if (DISPLAY_DEEP_SLEEP & suspend_status) {
		__wrn("ioctl:%x fail when in suspend!\n", cmd);
		return -1;
	}

	if (cmd == DISP_print)
		__wrn("cmd:0x%x,%ld,%ld\n", cmd, ubuffer[0], ubuffer[1]);

	switch (cmd) {
		/* ----disp global---- */
	case DISP_SET_BKCOLOR:
		{
			struct disp_color para;

			if (copy_from_user(&para, (void __user *)ubuffer[1],
			     sizeof(struct disp_color))) {
				__wrn("copy_from_user fail\n");
				return -EFAULT;
			}
			if (mgr && (mgr->set_back_color != NULL))
				ret = mgr->set_back_color(mgr, &para);
			break;
		}

	case DISP_GET_OUTPUT_TYPE:
		{
			if (suspend_status != DISPLAY_NORMAL)
				ret = suspend_output_type[ubuffer[0]];
			else
				ret = bsp_disp_get_output_type(ubuffer[0]);

			break;
		}

	case DISP_GET_SCN_WIDTH:
		{
			unsigned int width = 0, height = 0;

			if (mgr && mgr->device && mgr->device->get_resolution)
				mgr->device->get_resolution(mgr->device, &width,
							    &height);
			ret = width;
			break;
		}

	case DISP_GET_SCN_HEIGHT:
		{
			unsigned int width = 0, height = 0;

			if (mgr && mgr->device && mgr->device->get_resolution)
				mgr->device->get_resolution(mgr->device, &width,
							    &height);
			ret = height;
			break;
		}

	case DISP_VSYNC_EVENT_EN:
		{
			ret =
			    bsp_disp_vsync_event_enable(ubuffer[0], ubuffer[1]);
			break;
		}

	case DISP_SHADOW_PROTECT:
		{
			ret = bsp_disp_shadow_protect(ubuffer[0], ubuffer[1]);
			break;
		}

	case DISP_BLANK:
		{
			/* only response main device' blank request */

			if (!g_pm_runtime_enable)
				break;

			if (ubuffer[0] != 0)
				break;

			if (ubuffer[1]) {
#ifdef CONFIG_ARCH_SUN50IW6
			bsp_disp_hdmi_cec_standby_request();
#endif
#if defined(CONFIG_PM_RUNTIME)
				if (g_disp_drv.dev)
					pm_runtime_put(g_disp_drv.dev);
				else
					pr_warn("%s, display device is null\n",
						__func__);
#endif
				suspend_status |= DISPLAY_BLANK;
				disp_blank(true);
			} else {
				if (power_status_init) {
					/* avoid first unblank */
					power_status_init = 0;
					break;
				}

				disp_blank(false);
				suspend_status &= ~DISPLAY_BLANK;
#if defined(CONFIG_PM_RUNTIME)
				if (g_disp_drv.dev) {
					/* recover the pm_runtime status */
					pm_runtime_disable(g_disp_drv.dev);
					pm_runtime_set_suspended(g_disp_drv.
								 dev);
					pm_runtime_enable(g_disp_drv.dev);
					pm_runtime_get_sync(g_disp_drv.dev);
				} else
					pr_warn("%s, display device is null\n",
						__func__);
#endif
			}
			break;
		}

	case DISP_DEVICE_SWITCH:
		{
			/* if the display device has already enter blank status,
			 * DISP_DEVICE_SWITCH request will not be responsed.
			 */
			if (!(suspend_status & DISPLAY_BLANK))
				ret =
				    bsp_disp_device_switch(ubuffer[0],
					   (enum disp_output_type)ubuffer[1],
					   (enum disp_output_type)ubuffer[2]);
			suspend_output_type[ubuffer[0]] = ubuffer[1];
#if defined(SUPPORT_TV) && defined(CONFIG_ARCH_SUN50IW2P1)
			bsp_disp_tv_set_hpd(1);
#endif
		break;
	}

	case DISP_DEVICE_SET_CONFIG:
	{
		struct disp_device_config config;

		if (copy_from_user(&config, (void __user *)ubuffer[1],
			sizeof(struct disp_device_config))) {
			__wrn("copy_from_user fail\n");
			return  -EFAULT;
		}
		suspend_output_type[ubuffer[0]] = config.type;

		ret = bsp_disp_device_set_config(ubuffer[0], &config);
		break;
	}

	case DISP_DEVICE_GET_CONFIG:
	{
		struct disp_device_config config;

		if (mgr && dispdev)
			dispdev->get_static_config(dispdev, &config);
		else
			ret = -EFAULT;

		if (ret == 0) {
			if (copy_to_user((void __user *)ubuffer[1], &config,
				sizeof(struct disp_device_config))) {
				__wrn("copy_to_user fail\n");
				return  -EFAULT;
			}
		}
		break;
	}
#if defined(SUPPORT_EINK)

	case DISP_EINK_UPDATE:
		{
			s32 i = 0;
			struct area_info area;

			if (!eink_manager) {
				pr_err("there is no eink manager!\n");
				break;
			}

			memset(lyr_cfg, 0,
			       16 * sizeof(struct disp_layer_config));
			if (copy_from_user(lyr_cfg, (void __user *)ubuffer[3],
			     sizeof(struct disp_layer_config) * ubuffer[1])) {
				__wrn("copy_from_user fail\n");
				return -EFAULT;
			}

			memset(&area, 0, sizeof(struct area_info));
			if (copy_from_user(&area, (void __user *)ubuffer[0],
					   sizeof(struct area_info))) {
				__wrn("copy_from_user fail\n");
				return -EFAULT;
			}

			for (i = 0; i < ubuffer[1]; i++)
				__disp_config_transfer2inner(&eink_para[i],
								&lyr_cfg[i]);

			ret = bsp_disp_eink_update(eink_manager,
				(struct disp_layer_config_inner *)&eink_para[0],
				(unsigned int)ubuffer[1],
				(enum eink_update_mode)ubuffer[2], &area);
			break;
		}

	case DISP_EINK_UPDATE2:
		{
			s32 i = 0;
			struct area_info area;

			if (!eink_manager) {
				pr_err("there is no eink manager!\n");
				break;
			}

			memset(lyr_cfg2, 0,
			       16 * sizeof(struct disp_layer_config2));
			if (copy_from_user(lyr_cfg2, (void __user *)ubuffer[3],
			     sizeof(struct disp_layer_config2) * ubuffer[1])) {
				__wrn("copy_from_user fail\n");
				return -EFAULT;
			}

			memset(&area, 0, sizeof(struct area_info));
			if (copy_from_user(&area, (void __user *)ubuffer[0],
					   sizeof(struct area_info))) {
				__wrn("copy_from_user fail\n");
				return -EFAULT;
			}

			for (i = 0; i < ubuffer[1]; i++)
				__disp_config2_transfer2inner(&eink_para[i],
								&lyr_cfg2[i]);

			ret = bsp_disp_eink_update(eink_manager,
				(struct disp_layer_config_inner *)&eink_para[0],
				(unsigned int)ubuffer[1],
				(enum eink_update_mode)ubuffer[2], &area);
			break;
		}

	case DISP_EINK_SET_TEMP:
		{
			ret =
			    bsp_disp_eink_set_temperature(eink_manager,
							  ubuffer[0]);
			break;
		}
	case DISP_EINK_GET_TEMP:
		{
			ret = bsp_disp_eink_get_temperature(eink_manager);
			break;
		}
	case DISP_EINK_OVERLAP_SKIP:
		{
			ret = bsp_disp_eink_op_skip(eink_manager, ubuffer[0]);
			break;
		}
#endif

	case DISP_GET_OUTPUT:
		{
			struct disp_output para;

			memset(&para, 0, sizeof(struct disp_output));

			if (mgr && mgr->device) {
				para.type =
				    bsp_disp_get_output_type(ubuffer[0]);
				if (mgr->device->get_mode)
					para.mode =
					    mgr->device->get_mode(mgr->device);
			}

			if (copy_to_user((void __user *)ubuffer[1], &para,
			     sizeof(struct disp_output))) {
				__wrn("copy_from_user fail\n");
				return -EFAULT;
			}
			break;
		}

	case DISP_SET_COLOR_RANGE:
		{
			if (mgr && mgr->set_output_color_range)
				ret =
				    mgr->set_output_color_range(mgr,
								ubuffer[1]);

			break;
		}

	case DISP_GET_COLOR_RANGE:
		{
			if (mgr && mgr->get_output_color_range)
				ret = mgr->get_output_color_range(mgr);

			break;
		}

		/* ----layer---- */
	case DISP_LAYER_SET_CONFIG:
	{
		unsigned int i = 0;
		const unsigned int lyr_cfg_size = ARRAY_SIZE(lyr_cfg);

		if (ubuffer[2] > lyr_cfg_size) {
			__wrn("Total layer number is %d\n", lyr_cfg_size);
			return -EFAULT;
		}

		if (copy_from_user(lyr_cfg,
			(void __user *)ubuffer[1],
			sizeof(struct disp_layer_config) * ubuffer[2]))	{
			__wrn("copy_from_user fail\n");

			return  -EFAULT;
		}

		for (i = 0; (i < lyr_cfg_size) && (i < ubuffer[2]); ++i) {
			if (lyr_cfg[i].enable == 0) {
				memset(&(lyr_cfg[i].info), 0,
					sizeof(lyr_cfg[i].info));
			}
		}

#if !defined(CONFIG_EINK_PANEL_USED)
		if (mgr && mgr->set_layer_config)
			ret = mgr->set_layer_config(mgr, lyr_cfg, ubuffer[2]);
#endif
		break;
	}

	case DISP_LAYER_GET_CONFIG:
	{
		if (copy_from_user(lyr_cfg,
			(void __user *)ubuffer[1],
			sizeof(struct disp_layer_config) * ubuffer[2]))	{
			__wrn("copy_from_user fail\n");

			return  -EFAULT;
		}
		if (mgr && mgr->get_layer_config)
			ret = mgr->get_layer_config(mgr, lyr_cfg, ubuffer[2]);
		if (copy_to_user((void __user *)ubuffer[1],
			lyr_cfg,
			sizeof(struct disp_layer_config) * ubuffer[2]))	{
			__wrn("copy_to_user fail\n");

			return  -EFAULT;
		}
		break;
	}

	case DISP_LAYER_SET_CONFIG2:
	{
		struct disp_layer_config2 *pLyr_cfg2;
		unsigned int i = 0;
		const unsigned int lyr_cfg_size =
			ARRAY_SIZE(lyr_cfg2);

		/* adapt to multi thread call in case of disp 0 & 1 work together*/
		if (ubuffer[0] == 0)
			pLyr_cfg2 = lyr_cfg2;
		else
			pLyr_cfg2 = lyr_cfg2_1;

		if (copy_from_user(pLyr_cfg2,
		    (void __user *)ubuffer[1],
		    sizeof(struct disp_layer_config2) * ubuffer[2])) {
			__wrn("copy_from_user fail\n");

			return  -EFAULT;
		}

		for (i = 0; (i < lyr_cfg_size) && (i < ubuffer[2]); ++i) {
			if (pLyr_cfg2[i].enable == 0) {
				memset(&(pLyr_cfg2[i].info), 0,
					sizeof(pLyr_cfg2[i].info));
			}
		}

#if !defined(CONFIG_EINK_PANEL_USED)
		if (mgr && mgr->set_layer_config2)
			ret = mgr->set_layer_config2(mgr, pLyr_cfg2, ubuffer[2]);
#endif
		break;
	}

	case DISP_LAYER_GET_CONFIG2:
	{
		if (copy_from_user(lyr_cfg2,
		    (void __user *)ubuffer[1],
		    sizeof(struct disp_layer_config2) * ubuffer[2])) {
			__wrn("copy_from_user fail\n");

			return  -EFAULT;
		}
		if (mgr && mgr->get_layer_config2)
			ret = mgr->get_layer_config2(mgr, lyr_cfg2, ubuffer[2]);
		if (copy_to_user((void __user *)ubuffer[1],
			lyr_cfg2,
			sizeof(struct disp_layer_config2) * ubuffer[2])) {
			__wrn("copy_to_user fail\n");

			return  -EFAULT;
		}
		break;
	}
		/* ---- lcd --- */
	case DISP_LCD_SET_BRIGHTNESS:
		{
			if (dispdev && dispdev->set_bright)
				ret = dispdev->set_bright(dispdev, ubuffer[1]);
			break;
		}

	case DISP_LCD_GET_BRIGHTNESS:
		{
			if (dispdev && dispdev->get_bright)
				ret = dispdev->get_bright(dispdev);
			break;
		}
	case DISP_TV_SET_GAMMA_TABLE:
	{
		if (dispdev && (DISP_OUTPUT_TYPE_TV == dispdev->type)) {
			u32 *gamma_tbl = kmalloc(LCD_GAMMA_TABLE_SIZE,
						 GFP_KERNEL | __GFP_ZERO);
			u32 size = ubuffer[2];

			if (gamma_tbl == NULL) {
				__wrn("kmalloc fail\n");
				ret = -EFAULT;
				break;
			}

			size = (size > LCD_GAMMA_TABLE_SIZE) ?
			    LCD_GAMMA_TABLE_SIZE : size;
			if (copy_from_user(gamma_tbl, (void __user *)ubuffer[1],
					  size)) {
				__wrn("copy_from_user fail\n");
				kfree(gamma_tbl);
				ret = -EFAULT;

				break;
			}
			if (dispdev->set_gamma_tbl)
				ret = dispdev->set_gamma_tbl(dispdev, gamma_tbl,
							     size);
			kfree(gamma_tbl);
		}
		break;
	}

	case DISP_LCD_GAMMA_CORRECTION_ENABLE:
		{
			if (dispdev &&
				(dispdev->type == DISP_OUTPUT_TYPE_LCD)) {
				ret = dispdev->enable_gamma(dispdev);
			}
			break;
		}

	case DISP_LCD_GAMMA_CORRECTION_DISABLE:
	{
		if (dispdev && (dispdev->type == DISP_OUTPUT_TYPE_LCD))
			ret = dispdev->disable_gamma(dispdev);
		break;
	}

	case DISP_LCD_SET_GAMMA_TABLE:
	{
		if (dispdev && (dispdev->type == DISP_OUTPUT_TYPE_LCD)) {
			u32 *gamma_tbl = kmalloc(LCD_GAMMA_TABLE_SIZE,
						 GFP_KERNEL | __GFP_ZERO);
			u32 size = ubuffer[2];

			if (gamma_tbl == NULL) {
				__wrn("kmalloc fail\n");
				ret = -EFAULT;
				break;
			}

			size = (size > LCD_GAMMA_TABLE_SIZE) ?
			    LCD_GAMMA_TABLE_SIZE : size;
			if (copy_from_user(gamma_tbl, (void __user *)ubuffer[1],
					  size)) {
				__wrn("copy_from_user fail\n");
				kfree(gamma_tbl);
				ret = -EFAULT;

				break;
			}
			ret = dispdev->set_gamma_tbl(dispdev, gamma_tbl, size);
			kfree(gamma_tbl);
		}
		break;
	}


		/* ---- hdmi --- */
	case DISP_HDMI_SUPPORT_MODE:
		{
			ret =
			    bsp_disp_hdmi_check_support_mode(ubuffer[0],
							     ubuffer[1]);
			break;
		}

	case DISP_SET_TV_HPD:
		{
			ret = bsp_disp_tv_set_hpd(ubuffer[0]);
			break;
		}
#ifdef CONFIG_ARCH_SUN50IW6
	case DISP_CEC_ONE_TOUCH_PLAY:
	{
		ret = bsp_disp_hdmi_cec_send_one_touch_play();
		break;
	}
#endif
		/* ----enhance---- */
	case DISP_ENHANCE_ENABLE:
		{
			if (enhance && enhance->enable)
				ret = enhance->enable(enhance);
			break;
		}

	case DISP_ENHANCE_DISABLE:
		{
			if (enhance && enhance->disable)
				ret = enhance->disable(enhance);
			break;
		}

	case DISP_ENHANCE_DEMO_ENABLE:
		{
			if (enhance && enhance->demo_enable)
				ret = enhance->demo_enable(enhance);
			break;
		}

	case DISP_ENHANCE_DEMO_DISABLE:
		{
			if (enhance && enhance->demo_disable)
				ret = enhance->demo_disable(enhance);
			break;
		}

	case DISP_ENHANCE_SET_MODE:
		{
			if (enhance && enhance->set_mode)
				ret = enhance->set_mode(enhance, ubuffer[1]);
			break;
		}

	case DISP_ENHANCE_GET_MODE:
		{
			if (enhance && enhance->get_mode)
				ret = enhance->get_mode(enhance);
			break;
		}

		/* ---smart backlight -- */
	case DISP_SMBL_ENABLE:
		{
			if (smbl && smbl->enable)
				ret = smbl->enable(smbl);
			break;
		}

	case DISP_SMBL_DISABLE:
		{
			if (smbl && smbl->disable)
				ret = smbl->disable(smbl);
			break;
		}

	case DISP_SMBL_SET_WINDOW:
		{
			struct disp_rect rect;

			if (copy_from_user(&rect, (void __user *)ubuffer[1],
			     sizeof(struct disp_rect))) {
				__wrn("copy_from_user fail\n");
				return -EFAULT;
			}
			if (smbl && smbl->set_window)
				ret = smbl->set_window(smbl, &rect);
			break;
		}

		/* ---capture -- */
	case DISP_CAPTURE_START:
		{
			if (cptr && cptr->start)
				ret = cptr->start(cptr);
			break;
		}

	case DISP_CAPTURE_STOP:
		{
			if (cptr && cptr->stop)
				ret = cptr->stop(cptr);
			break;
		}

	case DISP_CAPTURE_COMMIT:
		{
			struct disp_capture_info info;

			if (copy_from_user(&info, (void __user *)ubuffer[1],
			     sizeof(struct disp_capture_info))) {
				__wrn("copy_from_user fail\n");
				return -EFAULT;
			}
			if (cptr && cptr->commmit)
				ret = cptr->commmit(cptr, &info);
			break;
		}
	case DISP_CAPTURE_COMMIT2:
	{
		struct disp_capture_info2 info;

		if (copy_from_user(&info,
				   (void __user *)ubuffer[1],
				   sizeof(struct disp_capture_info2)))	{
			__wrn("copy_from_user fail\n");
			return  -EFAULT;
		}
		if (cptr && cptr->commmit2)
			ret = cptr->commmit2(cptr, &info);
		break;
	}

		/* ----for test---- */
	case DISP_MEM_REQUEST:
		ret = disp_mem_request(ubuffer[0], ubuffer[1]);
		break;

	case DISP_MEM_RELEASE:
		ret = disp_mem_release(ubuffer[0]);
		break;

	case DISP_MEM_GETADR:
		return g_disp_mm[ubuffer[0]].mem_start;
#if defined(SUPPORT_VDPO)
	case DISP_VDPO_SET_CONFIG:
		{
			struct disp_vdpo_config vdpo_para;

			if (copy_from_user(
				&vdpo_para, (void __user *)ubuffer[1],
				sizeof(struct disp_vdpo_config) * ubuffer[2])) {
				__wrn("copy_from_user fail\n");
				return -EFAULT;
			}
			if (mgr && mgr->device)
				disp_vdpo_set_config(mgr->device, &vdpo_para);
			break;
		}
#endif /*endif SUPPORT_VDPO*/

	default:
		ret = disp_ioctl_extend(cmd, (unsigned long)ubuffer);
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long disp_compat_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	compat_uptr_t karg[4];
	unsigned long __user *ubuffer;

	if (copy_from_user
	    ((void *)karg, (void __user *)arg, 4 * sizeof(compat_uptr_t))) {
		__wrn("copy_from_user fail\n");
		return -EFAULT;
	}

	ubuffer = compat_alloc_user_space(4 * sizeof(unsigned long));
	if (!access_ok(VERIFY_WRITE, ubuffer, 4 * sizeof(unsigned long)))
		return -EFAULT;

	if (put_user(karg[0], &ubuffer[0]) ||
	    put_user(karg[1], &ubuffer[1]) ||
	    put_user(karg[2], &ubuffer[2]) || put_user(karg[3], &ubuffer[3])) {
		__wrn("put_user fail\n");
		return -EFAULT;
	}

	if (cmd == DISP_HWC_COMMIT)
		return disp_compat_ioctl_extend(cmd, (unsigned long)ubuffer);

	return disp_ioctl(file, cmd, (unsigned long)ubuffer);
}
#endif

static const struct file_operations disp_fops = {
	.owner          = THIS_MODULE,
	.open           = disp_open,
	.release        = disp_release,
	.write          = disp_write,
	.read           = disp_read,
	.unlocked_ioctl = disp_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = disp_compat_ioctl,
#endif
	.mmap           = disp_mmap,
};

#ifndef CONFIG_OF
static struct platform_device disp_device = {
	.name = "disp",
	.id = -1,
	.num_resources = ARRAY_SIZE(disp_resource),
	.resource = disp_resource,
	.dev = {
		.power = {
			  .async_suspend = 1,
			  }
		}
};
#else
static const struct of_device_id sunxi_disp_match[] = {
	{.compatible = "allwinner,sun8iw10p1-disp",},
	{.compatible = "allwinner,sun50i-disp",},
	{.compatible = "allwinner,sunxi-disp",},
	{},
};
#endif

static struct platform_driver disp_driver = {
	.probe    = disp_probe,
	.remove   = disp_remove,
	.shutdown = disp_shutdown,
	.driver   = {
		  .name           = "disp",
		  .owner          = THIS_MODULE,
		  .pm             = &disp_runtime_pm_ops,
		  .of_match_table = sunxi_disp_match,
	},
};

#ifdef CONFIG_DEVFREQ_DRAM_FREQ_IN_VSYNC
struct dramfreq_vb_time_ops {
	int (*get_vb_time)(void);
	int (*get_next_vb_time)(void);
	int (*is_in_vb)(void);
};
static struct dramfreq_vb_time_ops dramfreq_ops = {
	.get_vb_time = bsp_disp_get_vb_time,
	.get_next_vb_time = bsp_disp_get_next_vb_time,
	.is_in_vb = bsp_disp_is_in_vb,
};
extern int dramfreq_set_vb_time_ops(struct dramfreq_vb_time_ops *ops);
#endif

static int __init disp_module_init(void)
{
	int ret = 0, err;

	pr_info("[DISP]%s\n", __func__);

	alloc_chrdev_region(&devid, 0, 1, "disp");
	my_cdev = cdev_alloc();
	cdev_init(my_cdev, &disp_fops);
	my_cdev->owner = THIS_MODULE;
	err = cdev_add(my_cdev, devid, 1);
	if (err) {
		__wrn("cdev_add fail\n");
		return -1;
	}

	disp_class = class_create(THIS_MODULE, "disp");
	if (IS_ERR(disp_class)) {
		__wrn("class_create fail\n");
		return -1;
	}

	display_dev = device_create(disp_class, NULL, devid, NULL, "disp");

#ifndef CONFIG_OF
	ret = platform_device_register(&disp_device);
#endif
	if (ret == 0)
		ret = platform_driver_register(&disp_driver);
#ifdef CONFIG_DISP2_SUNXI_DEBUG
	dispdbg_init();
#endif

#ifdef CONFIG_DEVFREQ_DRAM_FREQ_IN_VSYNC
	dramfreq_set_vb_time_ops(&dramfreq_ops);
#endif

	pr_info("[DISP]%s finish\n", __func__);

	return ret;
}

static void __exit disp_module_exit(void)
{
	__inf("disp_module_exit\n");

#ifdef CONFIG_DISP2_SUNXI_DEBUG
	dispdbg_exit();
#endif

	disp_exit();

	platform_driver_unregister(&disp_driver);
#ifndef CONFIG_OF
	platform_device_unregister(&disp_device);
#endif

	device_destroy(disp_class, devid);
	class_destroy(disp_class);

	cdev_del(my_cdev);
}

module_init(disp_module_init);
module_exit(disp_module_exit);

MODULE_AUTHOR("tan");
MODULE_DESCRIPTION("display driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:disp");
