// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2016 NextThing Co
 * Copyright (C) 2016-2018 Bootlin
 *
 * Author: Maxime Ripard <maxime.ripard@bootlin.com>
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/videodev2.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mediabus.h>

#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "sun4i_csi.h"

static int csi_notify_bound(struct v4l2_async_notifier *notifier,
			    struct v4l2_subdev *subdev,
			    struct v4l2_async_subdev *asd)
{
	struct sun4i_csi *csi = container_of(notifier, struct sun4i_csi,
					     notifier);

	csi->src_subdev = subdev;
	csi->src_pad = media_entity_get_fwnode_pad(&subdev->entity,
						   subdev->fwnode,
						   MEDIA_PAD_FL_SOURCE);
	if (csi->src_pad < 0) {
		dev_err(csi->dev, "Couldn't find output pad for subdev %s\n",
			subdev->name);
		return csi->src_pad;
	}

	dev_dbg(csi->dev, "Bound %s pad: %d\n", subdev->name, csi->src_pad);
	return 0;
}

static int csi_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct sun4i_csi *csi = container_of(notifier, struct sun4i_csi,
					     notifier);
	int ret;

	if (notifier->num_subdevs != 1)
		return -EINVAL;

	ret = v4l2_device_register_subdev_nodes(&csi->v4l);
	if (ret < 0)
		return ret;

	ret = csi_v4l2_register(csi);
	if (ret < 0)
		return ret;

	return media_create_pad_link(&csi->src_subdev->entity, csi->src_pad,
				     &csi->vdev.entity, 0,
				     MEDIA_LNK_FL_ENABLED |
				     MEDIA_LNK_FL_IMMUTABLE);
}

static const struct v4l2_async_notifier_operations csi_notify_ops = {
	.bound		= csi_notify_bound,
	.complete	= csi_notify_complete,
};

static int sun4i_csi_async_parse(struct device *dev,
				 struct v4l2_fwnode_endpoint *vep,
				 struct v4l2_async_subdev *asd)
{
	struct sun4i_csi *csi = dev_get_drvdata(dev);

	if (vep->base.port || vep->base.id)
		return -EINVAL;

	if (vep->bus_type != V4L2_MBUS_PARALLEL)
		return -EINVAL;

	csi->bus = vep->bus.parallel;

	return 0;
}

static int csi_probe(struct platform_device *pdev)
{
	struct sun4i_csi *csi;
	struct resource *res;
	int ret;
	int irq;

	csi = devm_kzalloc(&pdev->dev, sizeof(*csi), GFP_KERNEL);
	if (!csi)
		return -ENOMEM;
	platform_set_drvdata(pdev, csi);
	csi->dev = &pdev->dev;

	csi->mdev.dev = csi->dev;
	strlcpy(csi->mdev.model, "Allwinner Video Capture Device",
		sizeof(csi->mdev.model));
	csi->mdev.hw_revision = 0;
	media_device_init(&csi->mdev);

	csi->pad.flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	ret = media_entity_pads_init(&csi->vdev.entity, 1, &csi->pad);
	if (ret < 0)
		return 0;

	csi->has_isp = of_property_read_bool(pdev->dev.of_node,
					     "allwinner,has-isp");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	csi->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(csi->regs))
		return PTR_ERR(csi->regs);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	csi->ahb_clk = devm_clk_get(&pdev->dev, "ahb");
	if (IS_ERR(csi->ahb_clk)) {
		dev_err(&pdev->dev, "Couldn't get our ahb clock\n");
		return PTR_ERR(csi->ahb_clk);
	}

	if (csi->has_isp) {
		csi->isp_clk = devm_clk_get(&pdev->dev, "isp");
		if (IS_ERR(csi->isp_clk)) {
			dev_err(&pdev->dev, "Couldn't get our ISP clock\n");
			return PTR_ERR(csi->isp_clk);
		}
	}

	csi->mod_clk = devm_clk_get(&pdev->dev, "mod");
	if (IS_ERR(csi->mod_clk)) {
		dev_err(&pdev->dev, "Couldn't get our mod clock\n");
		return PTR_ERR(csi->mod_clk);
	}

	csi->ram_clk = devm_clk_get(&pdev->dev, "ram");
	if (IS_ERR(csi->ram_clk)) {
		dev_err(&pdev->dev, "Couldn't get our ram clock\n");
		return PTR_ERR(csi->ram_clk);
	}

	csi->rst = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(csi->rst)) {
		dev_err(&pdev->dev, "Couldn't get our reset line\n");
		return PTR_ERR(csi->rst);
	}

	ret = csi_dma_register(csi, irq);
	if (ret)
		return ret;

	csi->v4l.mdev = &csi->mdev;

	ret = media_device_register(&csi->mdev);
	if (ret)
		goto err_unregister_dma;

	ret = v4l2_async_notifier_parse_fwnode_endpoints(csi->dev,
							 &csi->notifier,
							 sizeof(struct v4l2_async_subdev),
							 sun4i_csi_async_parse);
	if (ret)
		goto err_unregister_media;
	csi->notifier.ops = &csi_notify_ops;

	ret = v4l2_async_notifier_register(&csi->v4l, &csi->notifier);
	if (ret) {
		dev_err(csi->dev,
			"Couldn't register our v4l2-async notifier\n");
		goto err_free_notifier;
	}

	pm_runtime_enable(&pdev->dev);

	return 0;

err_free_notifier:
	v4l2_async_notifier_cleanup(&csi->notifier);

err_unregister_media:
	media_device_unregister(&csi->mdev);

err_unregister_dma:
	csi_dma_unregister(csi);
	return ret;
}

static int csi_remove(struct platform_device *pdev)
{
	struct sun4i_csi *csi = platform_get_drvdata(pdev);

	v4l2_async_notifier_unregister(&csi->notifier);
	v4l2_async_notifier_cleanup(&csi->notifier);
	media_device_unregister(&csi->mdev);
	csi_dma_unregister(csi);

	return 0;
}

static const struct of_device_id csi_of_match[] = {
	{ .compatible = "allwinner,sun4i-a10-csi" },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, csi_of_match);

static int csi_runtime_resume(struct device *dev)
{
	struct sun4i_csi *csi = dev_get_drvdata(dev);

	reset_control_deassert(csi->rst);
	clk_prepare_enable(csi->ahb_clk);
	clk_prepare_enable(csi->ram_clk);

	if (csi->has_isp) {
		clk_set_rate(csi->isp_clk, 80000000);
		clk_prepare_enable(csi->isp_clk);
	}

	clk_set_rate(csi->mod_clk, 24000000);
	clk_prepare_enable(csi->mod_clk);

	writel(1, csi->regs + CSI_EN_REG);

	return 0;
}

static int csi_runtime_suspend(struct device *dev)
{
	struct sun4i_csi *csi = dev_get_drvdata(dev);

	clk_disable_unprepare(csi->mod_clk);

	if (csi->has_isp)
		clk_disable_unprepare(csi->isp_clk);

	clk_disable_unprepare(csi->ram_clk);
	clk_disable_unprepare(csi->ahb_clk);

	reset_control_assert(csi->rst);

	return 0;
}

static const struct dev_pm_ops csi_pm_ops = {
	.runtime_resume		= csi_runtime_resume,
	.runtime_suspend	= csi_runtime_suspend,
};

static struct platform_driver csi_driver = {
	.probe	= csi_probe,
	.remove	= csi_remove,
	.driver	= {
		.name		= "sun4i-csi",
		.of_match_table	= csi_of_match,
		.pm		= &csi_pm_ops,
	},
};
module_platform_driver(csi_driver);
