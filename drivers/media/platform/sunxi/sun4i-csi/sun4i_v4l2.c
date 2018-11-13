// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2016 NextThing Co
 * Copyright (C) 2016-2018 Bootlin
 *
 * Author: Maxime Ripard <maxime.ripard@bootlin.com>
 */

#include <linux/device.h>
#include <linux/pm_runtime.h>

#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>

#include "sun4i_csi.h"

#define CSI_DEFAULT_FORMAT	V4L2_PIX_FMT_YUV420M
#define CSI_DEFAULT_WIDTH	640
#define CSI_DEFAULT_HEIGHT	480

#define CSI_MAX_HEIGHT		8192U
#define CSI_MAX_WIDTH		8192U

static const struct sun4i_csi_format csi_formats[] = {
	/* YUV422 inputs */
	{
		.mbus		= MEDIA_BUS_FMT_YUYV8_2X8,
		.fourcc		= V4L2_PIX_FMT_YUV420M,
		.input		= CSI_INPUT_YUV,
		.output		= CSI_OUTPUT_YUV_420_PLANAR,
		.num_planes	= 3,
		.bpp		= { 8, 8, 8 },
		.hsub		= 2,
		.vsub		= 2,
	},
};

static const struct sun4i_csi_format *
csi_get_format_by_fourcc(struct sun4i_csi *csi, u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(csi_formats); i++)
		if (csi_formats[i].fourcc == fourcc)
			return &csi_formats[i];

	return NULL;
}

static int csi_querycap(struct file *file, void *priv,
			struct v4l2_capability *cap)
{
	struct sun4i_csi *csi = video_drvdata(file);

	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strscpy(cap->card, "sun4i-csi", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(csi->dev));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int csi_enum_input(struct file *file, void *priv,
			  struct v4l2_input *inp)
{
	if (inp->index != 0)
		return -EINVAL;

	inp->type = V4L2_INPUT_TYPE_CAMERA;
	strlcpy(inp->name, "Camera", sizeof(inp->name));

	return 0;
}

static int csi_g_input(struct file *file, void *fh,
		       unsigned int *i)
{
	*i = 0;

	return 0;
}

static int csi_s_input(struct file *file, void *fh,
		       unsigned int i)
{
	if (i != 0)
		return -EINVAL;

	return 0;
}

static int _csi_try_fmt(struct sun4i_csi *csi,
			struct v4l2_pix_format_mplane *pix,
			const struct sun4i_csi_format **fmt)
{
	const struct sun4i_csi_format *_fmt;
	unsigned int width = pix->width;
	unsigned int height = pix->height;
	int i;

	_fmt = csi_get_format_by_fourcc(csi, pix->pixelformat);
	if (!_fmt)
		_fmt = csi_get_format_by_fourcc(csi, csi_formats[0].fourcc);

	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
	pix->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(pix->colorspace);
	pix->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(pix->colorspace);
	pix->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true, pix->colorspace,
							  pix->ycbcr_enc);

	pix->num_planes = _fmt->num_planes;
	pix->pixelformat = _fmt->fourcc;

	memset(pix->reserved, 0, sizeof(pix->reserved));

	/* Align the width and height on the subsampling */
	width = ALIGN(width, _fmt->hsub);
	height = ALIGN(height, _fmt->vsub);

	/* Clamp the width and height to our capabilities */
	pix->width = clamp(width, _fmt->hsub, CSI_MAX_WIDTH);
	pix->height = clamp(height, _fmt->vsub, CSI_MAX_HEIGHT);

	for (i = 0; i < _fmt->num_planes; i++) {
		unsigned int hsub = i > 0 ? _fmt->hsub : 1;
		unsigned int vsub = i > 0 ? _fmt->vsub : 1;
		unsigned int bpl;

		bpl = pix->width / hsub * _fmt->bpp[i] / 8;
		pix->plane_fmt[i].bytesperline = bpl;
		pix->plane_fmt[i].sizeimage = bpl * pix->height / vsub;
		memset(pix->plane_fmt[i].reserved, 0,
		       sizeof(pix->plane_fmt[i].reserved));
	}

	if (fmt)
		*fmt = _fmt;

	return 0;
}

static int csi_try_fmt_vid_cap(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct sun4i_csi *csi = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	return _csi_try_fmt(csi, &f->fmt.pix_mp, NULL);
}

static int csi_s_fmt_vid_cap(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	const struct sun4i_csi_format *fmt;
	struct sun4i_csi *csi = video_drvdata(file);
	int ret;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	ret = _csi_try_fmt(csi, &f->fmt.pix_mp, &fmt);
	if (ret)
		return ret;

	csi->v_fmt = f->fmt.pix_mp;
	csi->p_fmt = fmt;

	return 0;
}

static int csi_g_fmt_vid_cap(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct sun4i_csi *csi = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	f->fmt.pix_mp = csi->v_fmt;

	return 0;
}

static int csi_enum_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(csi_formats))
		return -EINVAL;

	f->pixelformat = csi_formats[f->index].fourcc;

	return 0;
}

static const struct v4l2_ioctl_ops csi_ioctl_ops = {
	.vidioc_querycap	= csi_querycap,

	.vidioc_enum_fmt_vid_cap_mplane	= csi_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap_mplane	= csi_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap_mplane	= csi_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap_mplane	= csi_try_fmt_vid_cap,

	.vidioc_enum_input	= csi_enum_input,
	.vidioc_g_input		= csi_g_input,
	.vidioc_s_input		= csi_s_input,

	.vidioc_reqbufs		= vb2_ioctl_reqbufs,
	.vidioc_create_bufs	= vb2_ioctl_create_bufs,
	.vidioc_querybuf	= vb2_ioctl_querybuf,
	.vidioc_qbuf		= vb2_ioctl_qbuf,
	.vidioc_dqbuf		= vb2_ioctl_dqbuf,
	.vidioc_expbuf		= vb2_ioctl_expbuf,
	.vidioc_prepare_buf	= vb2_ioctl_prepare_buf,
	.vidioc_streamon	= vb2_ioctl_streamon,
	.vidioc_streamoff	= vb2_ioctl_streamoff,
};

static int csi_open(struct file *file)
{
	struct sun4i_csi *csi = video_drvdata(file);
	int ret;

	pm_runtime_get_sync(csi->dev);

	ret = v4l2_subdev_call(csi->src_subdev, core, s_power, 1);
	if (ret < 0 && ret != -ENOIOCTLCMD)
		return ret;

	return v4l2_fh_open(file);
}

static int csi_release(struct file *file)
{
	struct sun4i_csi *csi = video_drvdata(file);

	pm_runtime_put(csi->dev);

	return vb2_fop_release(file);
}

static const struct v4l2_file_operations csi_fops = {
	.owner		= THIS_MODULE,
	.open		= csi_open,
	.release	= csi_release,
	.unlocked_ioctl	= video_ioctl2,
	.read		= vb2_fop_read,
	.write		= vb2_fop_write,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
};

int csi_v4l2_register(struct sun4i_csi *csi)
{
	struct video_device *vdev = &csi->vdev;
	int ret;

	vdev->v4l2_dev = &csi->v4l;
	vdev->queue = &csi->queue;
	strlcpy(vdev->name, KBUILD_MODNAME, sizeof(vdev->name));
	vdev->release = video_device_release_empty;
	vdev->lock = &csi->lock;

	/* Set a default format */
	csi->v_fmt.pixelformat = CSI_DEFAULT_FORMAT;
	csi->v_fmt.width = CSI_DEFAULT_WIDTH;
	csi->v_fmt.height = CSI_DEFAULT_HEIGHT;
	_csi_try_fmt(csi, &csi->v_fmt, NULL);

	vdev->fops = &csi_fops;
	vdev->ioctl_ops = &csi_ioctl_ops;
	video_set_drvdata(vdev, csi);

	ret = video_register_device(&csi->vdev, VFL_TYPE_GRABBER, -1);
	if (ret)
		return ret;

	dev_info(csi->dev, "Device registered as %s\n",
		 video_device_node_name(vdev));

	return 0;
}

