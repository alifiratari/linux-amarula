// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2016 NextThing Co
 * Copyright (C) 2016-2018 Bootlin
 *
 * Author: Maxime Ripard <maxime.ripard@bootlin.com>
 */

#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include "sun4i_csi.h"

struct csi_buffer {
	struct vb2_v4l2_buffer	vb;
	struct list_head	list;
};

static inline struct csi_buffer *vb2_v4l2_to_csi_buffer(const struct vb2_v4l2_buffer *p)
{
	return container_of(p, struct csi_buffer, vb);
}

static inline struct csi_buffer *vb2_to_csi_buffer(const struct vb2_buffer *p)
{
	return vb2_v4l2_to_csi_buffer(to_vb2_v4l2_buffer(p));
}

static void csi_capture_start(struct sun4i_csi *csi)
{
	writel(CSI_CPT_CTRL_VIDEO_START, csi->regs + CSI_CPT_CTRL_REG);
}

static void csi_capture_stop(struct sun4i_csi *csi)
{
	writel(0, csi->regs + CSI_CPT_CTRL_REG);
}

static int csi_queue_setup(struct vb2_queue *vq,
			   unsigned int *nbuffers,
			   unsigned int *nplanes,
			   unsigned int sizes[],
			   struct device *alloc_devs[])

{
	struct sun4i_csi *csi = vb2_get_drv_priv(vq);
	unsigned int i;

	if (*nbuffers < 3)
		*nbuffers = 3;

	*nplanes = csi->p_fmt->num_planes;

	for (i = 0; i < *nplanes; i++)
		sizes[i] = csi->v_fmt.plane_fmt[i].sizeimage;

	return 0;
};

static int csi_buffer_prepare(struct vb2_buffer *vb)
{
	struct sun4i_csi *csi = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int i;

	for (i = 0; i < csi->p_fmt->num_planes; i++) {
		unsigned long size = csi->v_fmt.plane_fmt[i].sizeimage;

		if (vb2_plane_size(vb, i) < size) {
			dev_err(csi->dev, "buffer too small (%lu < %lu)\n",
				vb2_plane_size(vb, i), size);
			return -EINVAL;
		}

		vb2_set_plane_payload(vb, i, size);
	}

	return 0;
}

static int csi_buffer_fill_slot(struct sun4i_csi *csi, unsigned int slot)
{
	struct csi_buffer *c_buf;
	struct vb2_v4l2_buffer *v_buf;
	unsigned int plane;

	/*
	 * We should never end up in a situation where we overwrite an
	 * already filled slot.
	 */
	if (WARN_ON(csi->current_buf[slot]))
		return -EINVAL;

	if (list_empty(&csi->buf_list)) {
		csi->current_buf[slot] = NULL;

		dev_warn(csi->dev, "Running out of buffers...\n");
		return -ENOMEM;
	}

	c_buf = list_first_entry(&csi->buf_list, struct csi_buffer, list);
	list_del_init(&c_buf->list);

	v_buf = &c_buf->vb;
	csi->current_buf[slot] = v_buf;

	for (plane = 0; plane < csi->p_fmt->num_planes; plane++) {
		dma_addr_t buf_addr;

		buf_addr = vb2_dma_contig_plane_dma_addr(&v_buf->vb2_buf,
							 plane);
		writel(buf_addr, csi->regs + CSI_BUF_ADDR_REG(plane, slot));
	}

	return 0;
}

static int csi_buffer_fill_all(struct sun4i_csi *csi)
{
	unsigned int slot;
	int ret;

	for (slot = 0; slot < CSI_MAX_BUFFER; slot++) {
		ret = csi_buffer_fill_slot(csi, slot);
		if (ret)
			return ret;
	}

	return 0;
}

static void csi_buffer_mark_done(struct sun4i_csi *csi,
				 unsigned int slot,
				 unsigned int sequence)
{
	struct vb2_v4l2_buffer *v_buf;

	if (WARN_ON(!csi->current_buf[slot]))
		return;

	v_buf = csi->current_buf[slot];
	v_buf->field = csi->v_fmt.field;
	v_buf->sequence = sequence;
	v_buf->vb2_buf.timestamp = ktime_get_ns();
	vb2_buffer_done(&v_buf->vb2_buf, VB2_BUF_STATE_DONE);

	csi->current_buf[slot] = NULL;
}

static int csi_buffer_flip(struct sun4i_csi *csi, unsigned int sequence)
{
	u32 reg = readl(csi->regs + CSI_BUF_CTRL_REG);
	int curr, next;

	/* Our next buffer is not the current buffer */
	curr = !!(reg & CSI_BUF_CTRL_DBS);
	next = !curr;

	/* Report the previous buffer as done */
	csi_buffer_mark_done(csi, next, sequence);

	/* Put a new buffer in there */
	return csi_buffer_fill_slot(csi, next);
}

static void csi_buffer_queue(struct vb2_buffer *vb)
{
	struct sun4i_csi *csi = vb2_get_drv_priv(vb->vb2_queue);
	struct csi_buffer *buf = vb2_to_csi_buffer(vb);
	unsigned long flags;

	spin_lock_irqsave(&csi->qlock, flags);
	list_add_tail(&buf->list, &csi->buf_list);
	spin_unlock_irqrestore(&csi->qlock, flags);
}

static void return_all_buffers(struct sun4i_csi *csi,
			       enum vb2_buffer_state state)
{
	struct csi_buffer *buf, *node;
	unsigned int slot;

	list_for_each_entry_safe(buf, node, &csi->buf_list, list) {
		vb2_buffer_done(&buf->vb.vb2_buf, state);
		list_del(&buf->list);
	}

	for (slot = 0; slot < CSI_MAX_BUFFER; slot++) {
		struct vb2_v4l2_buffer *v_buf = csi->current_buf[slot];

		if (!v_buf)
			continue;

		vb2_buffer_done(&v_buf->vb2_buf, state);
		csi->current_buf[slot] = NULL;
	}
}

static int csi_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct sun4i_csi *csi = vb2_get_drv_priv(vq);
	struct v4l2_fwnode_bus_parallel *bus = &csi->bus;
	unsigned long hsync_pol, pclk_pol, vsync_pol;
	unsigned long flags;
	int ret = 0;

	csi->sequence = 0;

	if (count < 2)
		return -ENOBUFS;

	ret = media_pipeline_start(&csi->vdev.entity, &csi->vdev.pipe);
	if (ret < 0)
		goto clear_dma_queue;

	dev_dbg(csi->dev, "Starting capture\n");

	spin_lock_irqsave(&csi->qlock, flags);

	/* Setup timings */
	writel(CSI_WIN_CTRL_W_ACTIVE(csi->v_fmt.width * 2),
	       csi->regs + CSI_WIN_CTRL_W_REG);
	writel(CSI_WIN_CTRL_H_ACTIVE(csi->v_fmt.height),
	       csi->regs + CSI_WIN_CTRL_H_REG);

	hsync_pol = !!(bus->flags & V4L2_MBUS_HSYNC_ACTIVE_HIGH);
	pclk_pol = !!(bus->flags & V4L2_MBUS_DATA_ACTIVE_HIGH);
	vsync_pol = !!(bus->flags & V4L2_MBUS_VSYNC_ACTIVE_HIGH);
	writel(CSI_CFG_INPUT_FMT(csi->p_fmt->input) |
	       CSI_CFG_OUTPUT_FMT(csi->p_fmt->output) |
	       CSI_CFG_VSYNC_POL(vsync_pol) |
	       CSI_CFG_HSYNC_POL(hsync_pol) |
	       CSI_CFG_PCLK_POL(pclk_pol),
	       csi->regs + CSI_CFG_REG);

	/* Setup buffer length */
	writel(csi->v_fmt.plane_fmt[0].bytesperline,
	       csi->regs + CSI_BUF_LEN_REG);

	/* Prepare our buffers in hardware */
	ret = csi_buffer_fill_all(csi);
	if (ret) {
		spin_unlock_irqrestore(&csi->qlock, flags);
		goto disable_pipeline;
	}

	/* Enable double buffering */
	writel(CSI_BUF_CTRL_DBE, csi->regs + CSI_BUF_CTRL_REG);

	/* Clear the pending interrupts */
	writel(CSI_INT_FRM_DONE, csi->regs + 0x34);

	/* Enable frame done interrupt */
	writel(CSI_INT_FRM_DONE, csi->regs + CSI_INT_EN_REG);

	csi_capture_start(csi);

	spin_unlock_irqrestore(&csi->qlock, flags);

	ret = v4l2_subdev_call(csi->src_subdev, video, s_stream, 1);
	if (ret < 0 && ret != -ENOIOCTLCMD)
		goto disable_pipeline;

	return 0;

disable_pipeline:
	media_pipeline_stop(&csi->vdev.entity);

clear_dma_queue:
	return_all_buffers(csi, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void csi_stop_streaming(struct vb2_queue *vq)
{
	struct sun4i_csi *csi = vb2_get_drv_priv(vq);
	unsigned long flags;

	dev_dbg(csi->dev, "Stopping capture\n");

	v4l2_subdev_call(csi->src_subdev, video, s_stream, 0);
	csi_capture_stop(csi);

	/* Release all active buffers */
	spin_lock_irqsave(&csi->qlock, flags);
	return_all_buffers(csi, VB2_BUF_STATE_ERROR);
	spin_unlock_irqrestore(&csi->qlock, flags);

	media_pipeline_stop(&csi->vdev.entity);
}

static struct vb2_ops csi_qops = {
	.queue_setup		= csi_queue_setup,
	.buf_prepare		= csi_buffer_prepare,
	.buf_queue		= csi_buffer_queue,
	.start_streaming	= csi_start_streaming,
	.stop_streaming		= csi_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static irqreturn_t csi_irq(int irq, void *data)
{
	struct sun4i_csi *csi = data;
	unsigned int sequence;
	u32 reg;

	reg = readl(csi->regs + CSI_INT_STA_REG);

	/* Acknowledge the interrupts */
	writel(reg, csi->regs + CSI_INT_STA_REG);

	sequence = csi->sequence++;

	if (!(reg & CSI_INT_FRM_DONE))
		goto out;

	spin_lock(&csi->qlock);
	if (csi_buffer_flip(csi, sequence)) {
		dev_warn(csi->dev, "%s: Flip failed\n", __func__);
		csi_capture_stop(csi);
	}
	spin_unlock(&csi->qlock);

out:
	return IRQ_HANDLED;
}

int csi_dma_register(struct sun4i_csi *csi, int irq)
{
	struct vb2_queue *q = &csi->queue;
	int ret;
	int i;

	ret = v4l2_device_register(csi->dev, &csi->v4l);
	if (ret) {
		dev_err(csi->dev, "Couldn't register the v4l2 device\n");
		return ret;
	}

	spin_lock_init(&csi->qlock);
	mutex_init(&csi->lock);

	INIT_LIST_HEAD(&csi->buf_list);
	for (i = 0; i < CSI_MAX_BUFFER; i++)
		csi->current_buf[i] = NULL;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	q->io_modes = VB2_MMAP;
	q->lock = &csi->lock;
	q->drv_priv = csi;
	q->buf_struct_size = sizeof(struct csi_buffer);
	q->ops = &csi_qops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->gfp_flags = GFP_DMA32;
	q->dev = csi->dev;

	ret = vb2_queue_init(q);
	if (ret < 0) {
		dev_err(csi->dev, "failed to initialize VB2 queue\n");
		return ret;
	}

	ret = devm_request_irq(csi->dev, irq, csi_irq, 0,
			       dev_name(csi->dev), csi);
	if (ret) {
		dev_err(csi->dev, "Couldn't register our interrupt\n");
		return ret;
	}

	return 0;
}

void csi_dma_unregister(struct sun4i_csi *csi)
{
	v4l2_device_unregister(&csi->v4l);
}

