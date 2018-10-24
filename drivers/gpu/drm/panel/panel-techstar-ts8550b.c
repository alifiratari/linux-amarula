// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019, Amarula Solutions.
 * Author: Jagan Teki <jagan@amarulasolutions.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct ts8550b {
	struct drm_panel	panel;
	struct mipi_dsi_device	*dsi;

	struct backlight_device *backlight;
	struct regulator	*dvdd;
	struct regulator	*avdd;
	struct gpio_desc	*reset;

	bool			is_enabled;
	bool			is_prepared;
};

static inline struct ts8550b *panel_to_ts8550b(struct drm_panel *panel)
{
	return container_of(panel, struct ts8550b, panel);
}

static inline int ts8550b_dcs_write_seq(struct ts8550b *ctx, const void *seq,
					size_t len)
{
	return mipi_dsi_dcs_write_buffer(ctx->dsi, seq, len);
};

#define ts8550b_dcs_write_seq_static(ctx, seq...)		\
	({							\
		static const u8 d[] = { seq };			\
		ts8550b_dcs_write_seq(ctx, d, ARRAY_SIZE(d));	\
	})

static void ts8550b_init_sequence(struct ts8550b *ctx)
{
	ts8550b_dcs_write_seq_static(ctx, MIPI_DCS_SOFT_RESET, 0x00);
	msleep(200);
	ts8550b_dcs_write_seq_static(ctx, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x11);
	ts8550b_dcs_write_seq_static(ctx, 0xD1, 0x11);
	ts8550b_dcs_write_seq_static(ctx, MIPI_DCS_EXIT_SLEEP_MODE, 0x00);
	msleep(200);
	ts8550b_dcs_write_seq_static(ctx, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x10);
	ts8550b_dcs_write_seq_static(ctx, 0xC0, 0xE9, 0x03);
	ts8550b_dcs_write_seq_static(ctx, 0xC1, 0x12, 0x02);
	ts8550b_dcs_write_seq_static(ctx, 0xC2, 0x07, 0x06);
	ts8550b_dcs_write_seq_static(ctx, 0xB0, 0x00, 0x0E, 0x15, 0x0F, 0x11,
				     0x08, 0x08, 0x08, 0x08, 0x23, 0x04, 0x13,
				     0x12, 0x2B, 0x34, 0x1F);
	ts8550b_dcs_write_seq_static(ctx, 0xB1, 0x00, 0x0E, 0x95, 0x0F, 0x13,
				     0x07, 0x09, 0x08, 0x08, 0x22, 0x04, 0x10,
				     0x0E, 0x2C, 0x34, 0x1F);
	ts8550b_dcs_write_seq_static(ctx, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x11);
	ts8550b_dcs_write_seq_static(ctx, 0xB0, 0x45);
	ts8550b_dcs_write_seq_static(ctx, 0xB1, 0x13);
	ts8550b_dcs_write_seq_static(ctx, 0xB2, 0x07);
	ts8550b_dcs_write_seq_static(ctx, 0xB3, 0x80);
	ts8550b_dcs_write_seq_static(ctx, 0xB5, 0x47);
	ts8550b_dcs_write_seq_static(ctx, 0xB7, 0x85);
	ts8550b_dcs_write_seq_static(ctx, 0xB8, 0x20);
	ts8550b_dcs_write_seq_static(ctx, 0xB9, 0x11);
	ts8550b_dcs_write_seq_static(ctx, 0xC1, 0x78);
	ts8550b_dcs_write_seq_static(ctx, 0xC2, 0x78);
	ts8550b_dcs_write_seq_static(ctx, 0xD0, 0x88);
	msleep(100);
	ts8550b_dcs_write_seq_static(ctx, 0xE0, 0x00, 0x00, 0x02);
	ts8550b_dcs_write_seq_static(ctx, 0xE1, 0x0B, 0x00, 0x0D, 0x00, 0x0C,
				     0x00, 0x0E, 0x00, 0x00, 0x44, 0x44);
	ts8550b_dcs_write_seq_static(ctx, 0xE2, 0x33, 0x33, 0x44, 0x44, 0x64,
				     0x00, 0x66, 0x00, 0x65, 0x00, 0x67, 0x00,
				     0x00);
	ts8550b_dcs_write_seq_static(ctx, 0xE3, 0x00, 0x00, 0x33, 0x33);
	ts8550b_dcs_write_seq_static(ctx, 0xE4, 0x44, 0x44);
	ts8550b_dcs_write_seq_static(ctx, 0xE5, 0x0C, 0x78, 0x3C, 0xA0, 0x0E,
				     0x78, 0x3C, 0xA0, 0x10, 0x78, 0x3C, 0xA0,
				     0x12, 0x78, 0x3C, 0xA0);
	ts8550b_dcs_write_seq_static(ctx, 0xE6, 0x00, 0x00, 0x33, 0x33);
	ts8550b_dcs_write_seq_static(ctx, 0xE7, 0x44, 0x44);
	ts8550b_dcs_write_seq_static(ctx, 0xE8, 0x0D, 0x78, 0x3C, 0xA0, 0x0F,
				     0x78, 0x3C, 0xA0, 0x11, 0x78, 0x3C, 0xA0,
				     0x13, 0x78, 0x3C, 0xA0);
	ts8550b_dcs_write_seq_static(ctx, 0xEB, 0x02, 0x02, 0x39, 0x39, 0xEE,
				     0x44, 0x00);
	ts8550b_dcs_write_seq_static(ctx, 0xEC, 0x00, 0x00);
	ts8550b_dcs_write_seq_static(ctx, 0xED, 0xFF, 0xF1, 0x04, 0x56, 0x72,
				     0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF3, 0x27,
				     0x65, 0x40, 0x1F, 0xFF);
	ts8550b_dcs_write_seq_static(ctx, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00);
	msleep(10);
	ts8550b_dcs_write_seq_static(ctx, MIPI_DCS_SET_DISPLAY_ON, 0x00);
	msleep(200);
}

static int ts8550b_prepare(struct drm_panel *panel)
{
	struct ts8550b *ctx = panel_to_ts8550b(panel);
	int ret;

	if (ctx->is_prepared)
		return 0;

	gpiod_set_value(ctx->reset, 0);
	msleep(20);

	ret = regulator_enable(ctx->dvdd);
	if (ret)
		return ret;
	msleep(20);

	ret = regulator_enable(ctx->avdd);
	if (ret)
		return ret;
	msleep(20);

	gpiod_set_value(ctx->reset, 1);
	msleep(20);

	gpiod_set_value(ctx->reset, 0);
	msleep(30);

	gpiod_set_value(ctx->reset, 1);
	msleep(150);

	ts8550b_init_sequence(ctx);

	ctx->is_prepared = true;

	return 0;
}

static int ts8550b_enable(struct drm_panel *panel)
{
	struct ts8550b *ctx = panel_to_ts8550b(panel);

	if (ctx->is_enabled)
		return 0;

	msleep(120);

	backlight_enable(ctx->backlight);
	ctx->is_enabled = true;

	return 0;
}

static int ts8550b_disable(struct drm_panel *panel)
{
	struct ts8550b *ctx = panel_to_ts8550b(panel);

	if (!ctx->is_enabled)
		return 0;

	backlight_disable(ctx->backlight);
	ctx->is_enabled = false;

	return 0;
}

static int ts8550b_unprepare(struct drm_panel *panel)
{
	struct ts8550b *ctx = panel_to_ts8550b(panel);
	int ret;

	if (!ctx->is_prepared)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	if (ret < 0)
		dev_err(panel->dev, "failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	if (ret < 0)
		dev_err(panel->dev, "failed to enter sleep mode: %d\n", ret);

	msleep(120);

	regulator_disable(ctx->dvdd);

	regulator_disable(ctx->avdd);

	gpiod_set_value(ctx->reset, 0);

	gpiod_set_value(ctx->reset, 1);

	gpiod_set_value(ctx->reset, 0);

	ctx->is_prepared = false;

	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock		= 27500,
	.vrefresh	= 60,

	.hdisplay	= 480,
	.hsync_start	= 480 + 38,
	.hsync_end	= 480 + 38 + 12,
	.htotal		= 480 + 38 + 12 + 12,

	.vdisplay	= 854,
	.vsync_start	= 854 + 18,
	.vsync_end	= 854 + 18 + 8,
	.vtotal		= 854 + 18 + 8 + 4,
};

static int ts8550b_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct ts8550b *ctx = panel_to_ts8550b(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	panel->connector->display_info.width_mm = 69;
	panel->connector->display_info.height_mm = 139;

	return 1;
}

static const struct drm_panel_funcs ts8550b_funcs = {
	.disable	= ts8550b_disable,
	.unprepare	= ts8550b_unprepare,
	.prepare	= ts8550b_prepare,
	.enable		= ts8550b_enable,
	.get_modes	= ts8550b_get_modes,
};

static int ts8550b_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device_node *np;
	struct ts8550b *ctx;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dsi = dsi;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = &dsi->dev;
	ctx->panel.funcs = &ts8550b_funcs;

	ctx->dvdd = devm_regulator_get(&dsi->dev, "dvdd");
	if (IS_ERR(ctx->dvdd)) {
		dev_err(&dsi->dev, "Couldn't get dvdd regulator\n");
		return PTR_ERR(ctx->dvdd);
	}

	ctx->avdd = devm_regulator_get(&dsi->dev, "avdd");
	if (IS_ERR(ctx->avdd)) {
		dev_err(&dsi->dev, "Couldn't get avdd regulator\n");
		return PTR_ERR(ctx->avdd);
	}

	ctx->reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset)) {
		dev_err(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(ctx->reset);
	}

	np = of_parse_phandle(dsi->dev.of_node, "backlight", 0);
	if (np) {
		ctx->backlight = of_find_backlight_by_node(np);
		of_node_put(np);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0)
		return ret;

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 2;

	return mipi_dsi_attach(dsi);
}

static int ts8550b_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct ts8550b *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	if (ctx->backlight)
		put_device(&ctx->backlight->dev);

	return 0;
}

static const struct of_device_id ts8550b_of_match[] = {
	{ .compatible = "techstar,ts8550b" },
	{ }
};
MODULE_DEVICE_TABLE(of, ts8550b_of_match);

static struct mipi_dsi_driver ts8550b_dsi_driver = {
	.probe		= ts8550b_dsi_probe,
	.remove		= ts8550b_dsi_remove,
	.driver = {
		.name		= "techstar-ts8550b",
		.of_match_table	= ts8550b_of_match,
	},
};
module_mipi_dsi_driver(ts8550b_dsi_driver);

MODULE_AUTHOR("Jagan Teki <jagan@amarulasolutions.com>");
MODULE_DESCRIPTION("Techstar TS8550B MIPI-DSI LCD Panel Driver");
MODULE_LICENSE("GPL v2");
