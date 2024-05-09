// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos SoC series Pablo driver
 *
 * Copyright (c) 2020 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "is-device-ischain.h"
#include "is-device-sensor.h"
#include "is-subdev-ctrl.h"
#include "is-config.h"
#include "is-param.h"
#include "is-video.h"
#include "is-type.h"

static int is_ischain_3ac_cfg(struct is_subdev *subdev,
	void *device_data,
	struct is_frame *frame,
	struct is_crop *incrop,
	struct is_crop *otcrop,
	IS_DECLARE_PMAP(pmap))
{
	return 0;
}

static int is_ischain_3ac_start(struct is_device_ischain *device,
	struct is_subdev *subdev,
	struct is_frame *frame,
	struct is_queue *queue,
	struct taa_param *taa_param,
	struct is_crop *otcrop,
	IS_DECLARE_PMAP(pmap))
{
	int ret = 0;
	struct param_dma_output *dma_output;
	u32 hw_format, hw_bitwidth, hw_order;
	u32 hw_sbwc = 0, hw_msb = 0;
	u32 flag_extra, flag_pixel_size;
	bool chg_format = false;

	FIMC_BUG(!queue);
	FIMC_BUG(!queue->framecfg.format);

	hw_format = queue->framecfg.format->hw_format;
	hw_order = queue->framecfg.format->hw_order;
	hw_bitwidth = queue->framecfg.format->hw_bitwidth;

	/* pixel type [0:5] : pixel size, [6:7] : extra */
	flag_pixel_size = queue->framecfg.hw_pixeltype & PIXEL_TYPE_SIZE_MASK;
	flag_extra = (queue->framecfg.hw_pixeltype & PIXEL_TYPE_EXTRA_MASK) >> PIXEL_TYPE_EXTRA_SHIFT;

	if (hw_format == DMA_INOUT_FORMAT_BAYER_PACKED
		&& flag_pixel_size == CAMERA_PIXEL_SIZE_13BIT) {
		hw_msb = MSB_OF_3AA_DMA_OUT + 1;
		hw_bitwidth = DMA_INOUT_BIT_WIDTH_13BIT;
		chg_format = true;
	} else if (hw_format == DMA_INOUT_FORMAT_BAYER) {
		hw_msb = hw_bitwidth;	/* consider signed format only */
		hw_bitwidth = DMA_INOUT_BIT_WIDTH_16BIT;
		chg_format = true;
	}

	if (flag_extra) {
		hw_sbwc = (SBWC_BASE_ALIGN_MASK_LLC_OFF | flag_extra);
		chg_format = true;
	}

	if (chg_format)
		mdbg_pframe("ot_crop[bitwidth %d msb %d sbwc 0x%x]\n",
				device, subdev, frame,
				hw_bitwidth, hw_msb,
				hw_sbwc);

	if ((otcrop->w != taa_param->otf_input.bayer_crop_width) ||
		(otcrop->h != taa_param->otf_input.bayer_crop_height)) {
		merr("bds output size is invalid((%d, %d) != (%d, %d))", device,
			otcrop->w,
			otcrop->h,
			taa_param->otf_input.bayer_crop_width,
			taa_param->otf_input.bayer_crop_height);
		ret = -EINVAL;
		goto p_err;
	}

	if (otcrop->x || otcrop->y) {
		mwarn("crop pos(%d, %d) is ignored", device, otcrop->x, otcrop->y);
		otcrop->x = 0;
		otcrop->y = 0;
	}

	dma_output = is_itf_g_param(device, frame, subdev->param_dma_ot);
	dma_output->cmd = DMA_OUTPUT_COMMAND_ENABLE;
	dma_output->format = hw_format;
	dma_output->order = hw_order;
	dma_output->bitwidth = hw_bitwidth;
	dma_output->msb = hw_msb;
	dma_output->sbwc_type = hw_sbwc;
	dma_output->width = otcrop->w;
	dma_output->height = otcrop->h;
	dma_output->crop_enable = 0;

	subdev->output.crop = *otcrop;

	set_bit(subdev->param_dma_ot, pmap);
	set_bit(IS_SUBDEV_RUN, &subdev->state);

p_err:
	return ret;
}

static int is_ischain_3ac_stop(struct is_device_ischain *device,
	struct is_subdev *subdev,
	struct is_frame *frame,
	IS_DECLARE_PMAP(pmap))
{
	int ret = 0;
	struct param_dma_output *dma_output;

	mdbgd_ischain("%s\n", device, __func__);

	dma_output = is_itf_g_param(device, frame, subdev->param_dma_ot);
	dma_output->cmd = DMA_OUTPUT_COMMAND_DISABLE;

	set_bit(subdev->param_dma_ot, pmap);
	clear_bit(IS_SUBDEV_RUN, &subdev->state);

	return ret;
}

static int is_ischain_3ac_tag(struct is_subdev *subdev,
	void *device_data,
	struct is_frame *ldr_frame,
	struct camera2_node *node)
{
	int ret = 0;
	struct is_subdev *leader;
	struct is_queue *queue;
	struct taa_param *taa_param;
	struct is_crop *otcrop, otparm;
	struct is_device_ischain *device;
	IS_DECLARE_PMAP(pmap);
	u32 pixelformat = 0;

	device = (struct is_device_ischain *)device_data;

	FIMC_BUG(!device);
	FIMC_BUG(!device->is_region);
	FIMC_BUG(!subdev);
	FIMC_BUG(!GET_SUBDEV_QUEUE(subdev));
	FIMC_BUG(!ldr_frame);
	FIMC_BUG(!ldr_frame->shot);

	mdbgs_ischain(4, "3AAC TAG(request %d)\n", device, node->request);

	IS_INIT_PMAP(pmap);
	leader = subdev->leader;
	taa_param = &device->is_region->parameter.taa;
	queue = GET_SUBDEV_QUEUE(subdev);
	if (!queue) {
		merr("queue is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	if (!queue->framecfg.format) {
		merr("format is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	pixelformat = queue->framecfg.format->pixelformat;

	if (node->request) {
		otcrop = (struct is_crop *)node->output.cropRegion;

		otparm.x = 0;
		otparm.y = 0;
		otparm.w = taa_param->vdma4_output.width;
		otparm.h = taa_param->vdma4_output.height;

		if (IS_NULL_CROP(otcrop))
			*otcrop = otparm;

		if (!COMPARE_CROP(otcrop, &otparm) ||
			!test_bit(IS_SUBDEV_RUN, &subdev->state) ||
			test_bit(IS_SUBDEV_FORCE_SET, &leader->state)) {
			ret = is_ischain_3ac_start(device,
				subdev,
				ldr_frame,
				queue,
				taa_param,
				otcrop,
				pmap);
			if (ret) {
				merr("is_ischain_3ac_start is fail(%d)", device, ret);
				goto p_err;
			}

			mdbg_pframe("ot_crop[%d, %d, %d, %d]\n", device, subdev, ldr_frame,
				otcrop->x, otcrop->y, otcrop->w, otcrop->h);
		}

		ret = is_ischain_buf_tag(device,
			subdev,
			ldr_frame,
			pixelformat,
			otcrop->w,
			otcrop->h,
			ldr_frame->txcTargetAddress);
		if (ret) {
			mswarn("%d frame is drop", device, subdev, ldr_frame->fcount);
			node->request = 0;
		}
	} else {
		if (test_bit(IS_SUBDEV_RUN, &subdev->state)) {
			ret = is_ischain_3ac_stop(device,
				subdev,
				ldr_frame,
				pmap);
			if (ret) {
				merr("is_ischain_3ac_stop is fail(%d)", device, ret);
				goto p_err;
			}

			mdbg_pframe(" off\n", device, subdev, ldr_frame);
		}

		ldr_frame->txcTargetAddress[0] = 0;
		ldr_frame->txcTargetAddress[1] = 0;
		ldr_frame->txcTargetAddress[2] = 0;
		node->request = 0;
	}

	ret = is_itf_s_param(device, ldr_frame, pmap);
	if (ret) {
		mrerr("is_itf_s_param is fail(%d)", device, ldr_frame, ret);
		goto p_err;
	}

p_err:
	return ret;
}

const struct is_subdev_ops is_subdev_3ac_ops = {
	.bypass			= NULL,
	.cfg			= is_ischain_3ac_cfg,
	.tag			= is_ischain_3ac_tag,
};
