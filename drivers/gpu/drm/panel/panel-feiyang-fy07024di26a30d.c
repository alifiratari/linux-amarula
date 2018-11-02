// SPDX-License-Identifier: (GPL-2.0+ or MIT)
/*
 * Copyright (C) 2018 Amarula Solutions
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

struct fy07024di26a30d {
	struct drm_panel	panel;
	struct mipi_dsi_device	*dsi;

	struct backlight_device	*backlight;
	struct regulator	*dvdd;
	struct regulator	*avdd;
	struct gpio_desc	*reset;

	bool			is_enabled;
	bool			is_prepared;
};

static inline struct fy07024di26a30d *panel_to_fy07024di26a30d(struct drm_panel *panel)
{
	return container_of(panel, struct fy07024di26a30d, panel);
}

struct fy07024di26a30d_init_cmd {
	size_t len;
	const char *data;
};

#define FY07024DI26A30D(...) { \
	.len = sizeof((char[]){__VA_ARGS__}), \
	.data = (char[]){__VA_ARGS__} }

static const struct fy07024di26a30d_init_cmd fy07024di26a30d_init_cmds[] = {
	FY07024DI26A30D(0x80, 0x58),
	FY07024DI26A30D(0x81, 0x47),
	FY07024DI26A30D(0x82, 0xD4),
	FY07024DI26A30D(0x83, 0x88),
	FY07024DI26A30D(0x84, 0xA9),
	FY07024DI26A30D(0x85, 0xC3),
	FY07024DI26A30D(0x86, 0x82),
};

static int fy07024di26a30d_prepare(struct drm_panel *panel)
{
	struct fy07024di26a30d *ctx = panel_to_fy07024di26a30d(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	unsigned int i;
	int ret;

	if (ctx->is_prepared)
		return 0;

	gpiod_set_value(ctx->reset, 1);
	msleep(50);

	gpiod_set_value(ctx->reset, 0);
	msleep(20);

	gpiod_set_value(ctx->reset, 1);
	msleep(200);

	for (i = 0; i < ARRAY_SIZE(fy07024di26a30d_init_cmds); i++) {
		const struct fy07024di26a30d_init_cmd *cmd =
						&fy07024di26a30d_init_cmds[i];

		ret = mipi_dsi_dcs_write_buffer(dsi, cmd->data, cmd->len);
		if (ret < 0)
			return ret;
	}

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(panel->dev, "failed to set display on: %d\n", ret);
		return ret;
	}

	ctx->is_prepared = true;

	return 0;
}

static int fy07024di26a30d_enable(struct drm_panel *panel)
{
	struct fy07024di26a30d *ctx = panel_to_fy07024di26a30d(panel);

	if (ctx->is_enabled)
		return 0;

	msleep(120);

	backlight_enable(ctx->backlight);
	ctx->is_enabled = true;

	return 0;
}

static int fy07024di26a30d_disable(struct drm_panel *panel)
{
	struct fy07024di26a30d *ctx = panel_to_fy07024di26a30d(panel);

	if (!ctx->is_enabled)
		return 0;

	backlight_disable(ctx->backlight);
	ctx->is_enabled = false;

	return 0;
}

static int fy07024di26a30d_unprepare(struct drm_panel *panel)
{
	struct fy07024di26a30d *ctx = panel_to_fy07024di26a30d(panel);
	int ret;

	if (!ctx->is_prepared)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	if (ret < 0)
		dev_err(panel->dev, "failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	if (ret < 0)
		dev_err(panel->dev, "failed to enter sleep mode: %d\n", ret);

	msleep(100);

	regulator_disable(ctx->avdd);

	regulator_disable(ctx->dvdd);

	gpiod_set_value(ctx->reset, 0);

	gpiod_set_value(ctx->reset, 1);

	gpiod_set_value(ctx->reset, 0);

	ctx->is_prepared = false;

	return 0;
}

static const struct drm_display_mode fy07024di26a30d_default_mode = {
	.clock = 55000,
	.vrefresh = 60,

	.hdisplay = 1024,
	.hsync_start = 1024 + 396,
	.hsync_end = 1024 + 396 + 20,
	.htotal = 1024 + 396 + 20 + 100,

	.vdisplay = 600,
	.vsync_start = 600 + 12,
	.vsync_end = 600 + 12 + 2,
	.vtotal = 600 + 12 + 2 + 21,
};

static int fy07024di26a30d_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct fy07024di26a30d *ctx = panel_to_fy07024di26a30d(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &fy07024di26a30d_default_mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "failed to add mode %ux%ux@%u\n",
			fy07024di26a30d_default_mode.hdisplay,
			fy07024di26a30d_default_mode.vdisplay,
			fy07024di26a30d_default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs fy07024di26a30d_funcs = {
	.disable = fy07024di26a30d_disable,
	.unprepare = fy07024di26a30d_unprepare,
	.prepare = fy07024di26a30d_prepare,
	.enable = fy07024di26a30d_enable,
	.get_modes = fy07024di26a30d_get_modes,
};

static int fy07024di26a30d_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device_node *np;
	struct fy07024di26a30d *ctx;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dsi = dsi;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = &dsi->dev;
	ctx->panel.funcs = &fy07024di26a30d_funcs;

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

	ret = regulator_enable(ctx->dvdd);
	if (ret)
		return ret;

	msleep(100);

	ret = regulator_enable(ctx->avdd);
	if (ret)
		return ret;

	msleep(5);

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

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 4;

	return mipi_dsi_attach(dsi);
}

static int fy07024di26a30d_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct fy07024di26a30d *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	if (ctx->backlight)
		put_device(&ctx->backlight->dev);

	return 0;
}

static const struct of_device_id fy07024di26a30d_of_match[] = {
	{ .compatible = "feiyang,fy07024di26a30d", },
	{ }
};
MODULE_DEVICE_TABLE(of, fy07024di26a30d_of_match);

static struct mipi_dsi_driver fy07024di26a30d_driver = {
	.probe = fy07024di26a30d_dsi_probe,
	.remove = fy07024di26a30d_dsi_remove,
	.driver = {
		.name = "feiyang-fy07024di26a30d",
		.of_match_table = fy07024di26a30d_of_match,
	},
};
module_mipi_dsi_driver(fy07024di26a30d_driver);

MODULE_AUTHOR("Jagan Teki <jagan@amarulasolutions.com>");
MODULE_DESCRIPTION("Feiyang FY07024DI26A30-D MIPI-DSI LCD panel");
MODULE_LICENSE("GPL v2");
