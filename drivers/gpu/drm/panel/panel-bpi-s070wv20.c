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

struct s070wv20 {
	struct drm_panel	panel;
	struct mipi_dsi_device	*dsi;

	struct backlight_device	*backlight;
	struct regulator	*power;
	struct gpio_desc	*reset;
};

static inline struct s070wv20 *panel_to_s070wv20(struct drm_panel *panel)
{
	return container_of(panel, struct s070wv20, panel);
}

struct s070wv20_instr {
	u8	cmd;
	u8	data;
};

#define S070WV20_COMMAND_INSTR(_cmd, _data)	\
	{					\
		.cmd = (_cmd),			\
		.data = (_data),		\
	}

static const struct s070wv20_instr s070wv20_init[] = {
	S070WV20_COMMAND_INSTR(0x7a, 0xc1),
	S070WV20_COMMAND_INSTR(0x20, 0x20),
	S070WV20_COMMAND_INSTR(0x21, 0xe0),
	S070WV20_COMMAND_INSTR(0x22, 0x13),
	S070WV20_COMMAND_INSTR(0x23, 0x28),
	S070WV20_COMMAND_INSTR(0x24, 0x30),
	S070WV20_COMMAND_INSTR(0x25, 0x28),
	S070WV20_COMMAND_INSTR(0x26, 0x00),
	S070WV20_COMMAND_INSTR(0x27, 0x0d),
	S070WV20_COMMAND_INSTR(0x28, 0x03),
	S070WV20_COMMAND_INSTR(0x29, 0x1d),
	S070WV20_COMMAND_INSTR(0x34, 0x80),
	S070WV20_COMMAND_INSTR(0x36, 0x28),
	S070WV20_COMMAND_INSTR(0xb5, 0xa0),
	S070WV20_COMMAND_INSTR(0x5c, 0xff),
	S070WV20_COMMAND_INSTR(0x2a, 0x01),
	S070WV20_COMMAND_INSTR(0x56, 0x92),
	S070WV20_COMMAND_INSTR(0x6b, 0x71),
	S070WV20_COMMAND_INSTR(0x69, 0x2b),
	S070WV20_COMMAND_INSTR(0x10, 0x40),
	S070WV20_COMMAND_INSTR(0x11, 0x98),
	S070WV20_COMMAND_INSTR(0xb6, 0x20),
	S070WV20_COMMAND_INSTR(0x51, 0x20),
	S070WV20_COMMAND_INSTR(0x14, 0x43),
	S070WV20_COMMAND_INSTR(0x2a, 0x49),
	S070WV20_COMMAND_INSTR(0x09, 0x10),
};

static int s070wv20_send_cmd_data(struct s070wv20 *ctx, u8 cmd, u8 data)
{
	u8 buf[2] = { cmd, data };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int s070wv20_prepare(struct drm_panel *panel)
{
	struct s070wv20 *ctx = panel_to_s070wv20(panel);
	unsigned int i;
	int ret;

	/* Power the panel */
	ret = regulator_enable(ctx->power);
	if (ret)
		return ret;
	msleep(5);

	/* And reset it */
	gpiod_set_value(ctx->reset, 1);
	msleep(50);

	gpiod_set_value(ctx->reset, 0);
	msleep(50);

	for (i = 0; i < ARRAY_SIZE(s070wv20_init); i++) {
		const struct s070wv20_instr *instr = &s070wv20_init[i];

		ret = s070wv20_send_cmd_data(ctx, instr->cmd, instr->data);
		if (ret)
			return ret;

		msleep(10);
	}

	ret = mipi_dsi_dcs_set_tear_on(ctx->dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret)
		return ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
	if (ret)
		return ret;

	return 0;
}

static int s070wv20_enable(struct drm_panel *panel)
{
	struct s070wv20 *ctx = panel_to_s070wv20(panel);

	msleep(200);

	mipi_dsi_dcs_set_display_on(ctx->dsi);
	backlight_enable(ctx->backlight);

	return 0;
}

static int s070wv20_disable(struct drm_panel *panel)
{
	struct s070wv20 *ctx = panel_to_s070wv20(panel);

	backlight_disable(ctx->backlight);
	return mipi_dsi_dcs_set_display_off(ctx->dsi);
}

static int s070wv20_unprepare(struct drm_panel *panel)
{
	struct s070wv20 *ctx = panel_to_s070wv20(panel);

	mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	regulator_disable(ctx->power);
	gpiod_set_value(ctx->reset, 1);

	return 0;
}

static const struct drm_display_mode s070wv20_default_mode = {
	.clock = 30000,
	.vrefresh = 60,

	.hdisplay = 800,
	.hsync_start = 800 + 40,
	.hsync_end = 800 + 40 + 48,
	.htotal = 800 + 40 + 48 + 40,

	.vdisplay = 480,
	.vsync_start = 480 + 13,
	.vsync_end = 480 + 13 + 3,
	.vtotal = 480 + 13 + 3 + 29,
};

static int s070wv20_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct s070wv20 *ctx = panel_to_s070wv20(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &s070wv20_default_mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "failed to add mode %ux%ux@%u\n",
			s070wv20_default_mode.hdisplay,
			s070wv20_default_mode.vdisplay,
			s070wv20_default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	panel->connector->display_info.width_mm = 86;
	panel->connector->display_info.height_mm = 154;

	return 1;
}

static const struct drm_panel_funcs s070wv20_funcs = {
	.disable = s070wv20_disable,
	.unprepare = s070wv20_unprepare,
	.prepare = s070wv20_prepare,
	.enable = s070wv20_enable,
	.get_modes = s070wv20_get_modes,
};

static int s070wv20_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device_node *np;
	struct s070wv20 *ctx;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dsi = dsi;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = &dsi->dev;
	ctx->panel.funcs = &s070wv20_funcs;

	ctx->power = devm_regulator_get(&dsi->dev, "power");
	if (IS_ERR(ctx->power)) {
		dev_err(&dsi->dev, "Couldn't get our power regulator\n");
		return PTR_ERR(ctx->power);
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

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 4;

	return mipi_dsi_attach(dsi);
}

static int s070wv20_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct s070wv20 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	if (ctx->backlight)
		put_device(&ctx->backlight->dev);

	return 0;
}

static const struct of_device_id s070wv20_of_match[] = {
	{ .compatible = "bananapi,s070wv20-ct16", },
	{ }
};
MODULE_DEVICE_TABLE(of, s070wv20_of_match);

static struct mipi_dsi_driver s070wv20_driver = {
	.probe = s070wv20_dsi_probe,
	.remove = s070wv20_dsi_remove,
	.driver = {
		.name = "bananapi-s070wv20-ct16",
		.of_match_table = s070wv20_of_match,
	},
};
module_mipi_dsi_driver(s070wv20_driver);

MODULE_AUTHOR("Jagan Teki <jagan@amarulasolutions.com>");
MODULE_DESCRIPTION("Bananapi S070WV20-CT16 MIPI-DSI");
MODULE_LICENSE("GPL v2");
