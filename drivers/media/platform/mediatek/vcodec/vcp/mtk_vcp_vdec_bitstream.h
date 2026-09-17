/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MTK_VCP_VDEC_BITSTREAM_H
#define MTK_VCP_VDEC_BITSTREAM_H

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include "mtk_vcp_vp9_bitstream.h"

/* A bounded SPS-prefix reader, not a complete codec parser. The frontend can
 * only deliver 8-bit 4:2:0. Reject incompatible sequence headers before the
 * firmware sees them, including headers replacing an already running sequence.
 * The remaining syntax and parameter-set references are still firmware-owned.
 */
struct vcp_rbsp {
	const u8 *data;
	size_t size, pos;
	u32 byte, bits, zeros;
	bool error;
};

static inline u32 vcp_rbsp_bits(struct vcp_rbsp *r, unsigned int count)
{
	u32 value = 0;

	while (count-- && !r->error) {
		if (!r->bits) {
			if (r->pos >= r->size)
				goto truncated;
			r->byte = r->data[r->pos++];
			if (r->zeros == 2 && r->byte == 3) {
				if (r->pos >= r->size || r->data[r->pos] > 3)
					goto truncated;
				r->byte = r->data[r->pos++];
				r->zeros = 0;
			}
			r->zeros = r->byte ? 0 : (r->zeros < 2 ? r->zeros + 1 : 2);
			r->bits = 8;
		}
		value = (value << 1) | ((r->byte >> --r->bits) & 1);
	}
	return value;

truncated:
	r->error = true;
	return 0;
}

static inline u32 vcp_rbsp_ue(struct vcp_rbsp *r)
{
	unsigned int zeros = 0;
	u32 suffix;

	while (!vcp_rbsp_bits(r, 1)) {
		if (r->error || ++zeros > 31) {
			r->error = true;
			return 0;
		}
	}
	/* Separate the read from the expression: it changes the reader state. */
	suffix = vcp_rbsp_bits(r, zeros);
	return ((1U << zeros) - 1) + suffix;
}

static inline int vcp_h264_sps_guard(const u8 *data, size_t size)
{
	struct vcp_rbsp r = { .data = data, .size = size };
	u32 profile, constraints, id, chroma = 1, luma = 0, colour = 0;

	profile = vcp_rbsp_bits(&r, 8);
	constraints = vcp_rbsp_bits(&r, 8);
	vcp_rbsp_bits(&r, 8); /* level_idc */
	id = vcp_rbsp_ue(&r);
	if (r.error || (constraints & 3) || id > 31)
		return -EINVAL;

	switch (profile) {
	case 66: /* Baseline */
	case 77: /* Main */
	case 88: /* Extended: implicit 8-bit 4:2:0 */
		break;
	case 100: /* High */
		chroma = vcp_rbsp_ue(&r);
		if (r.error)
			return -EINVAL;
		if (chroma != 1)
			return -EOPNOTSUPP;
		luma = vcp_rbsp_ue(&r);
		colour = vcp_rbsp_ue(&r);
		break;
	default:
		/* High 10/422/444, scalable and multiview paths are not supported. */
		return -EOPNOTSUPP;
	}
	if (r.error)
		return -EINVAL;
	return luma || colour ? -EOPNOTSUPP : 0;
}

static inline int vcp_hevc_sps_guard(const u8 *data, size_t size)
{
	struct vcp_rbsp r = { .data = data, .size = size };
	u32 layers, profile, space, id, chroma, width, height, luma, colour;
	bool sub_profile[7] = {}, sub_level[7] = {};
	unsigned int i;

	vcp_rbsp_bits(&r, 4); /* sps_video_parameter_set_id */
	layers = vcp_rbsp_bits(&r, 3); /* sps_max_sub_layers_minus1 */
	vcp_rbsp_bits(&r, 1); /* sps_temporal_id_nesting_flag */
	if (r.error || layers > 6)
		return -EINVAL;
	space = vcp_rbsp_bits(&r, 2);
	vcp_rbsp_bits(&r, 1); /* general_tier_flag */
	profile = vcp_rbsp_bits(&r, 5);
	/* The rest of general_profile occupies 80 bits for every profile. */
	vcp_rbsp_bits(&r, 32);
	vcp_rbsp_bits(&r, 32);
	vcp_rbsp_bits(&r, 16);
	vcp_rbsp_bits(&r, 8); /* general_level_idc */
	for (i = 0; i < layers; i++) {
		sub_profile[i] = vcp_rbsp_bits(&r, 1);
		sub_level[i] = vcp_rbsp_bits(&r, 1);
	}
	if (layers)
		for (i = layers; i < 8; i++)
			if (vcp_rbsp_bits(&r, 2))
				return -EINVAL;
	for (i = 0; i < layers; i++) {
		if (sub_profile[i]) {
			vcp_rbsp_bits(&r, 32);
			vcp_rbsp_bits(&r, 32);
			vcp_rbsp_bits(&r, 24);
		}
		if (sub_level[i])
			vcp_rbsp_bits(&r, 8);
	}
	id = vcp_rbsp_ue(&r);
	chroma = vcp_rbsp_ue(&r);
	if (r.error || id > 15 || chroma > 3)
		return -EINVAL;
	if (space || profile < 1 || profile > 3 || chroma != 1)
		return -EOPNOTSUPP;
	width = vcp_rbsp_ue(&r);
	height = vcp_rbsp_ue(&r);
	if (vcp_rbsp_bits(&r, 1)) /* conformance_window_flag */
		for (i = 0; i < 4; i++)
			vcp_rbsp_ue(&r);
	luma = vcp_rbsp_ue(&r);
	colour = vcp_rbsp_ue(&r);
	if (r.error || !width || !height)
		return -EINVAL;
	return luma || colour ? -EOPNOTSUPP : 0;
}

static inline int vcp_vdec_nal_guard(u32 fourcc, const u8 *data, size_t size)
{
	u32 type;

	if (!size || (data[0] & 0x80))
		return -EINVAL;
	if (fourcc == V4L2_PIX_FMT_H264) {
		type = data[0] & 0x1f;
		if (type == 7)
			return vcp_h264_sps_guard(data + 1, size - 1);
		if (type == 14 || type == 15 || type == 20 || type == 21)
			return -EOPNOTSUPP;
		return 0;
	}
	if (fourcc != V4L2_PIX_FMT_HEVC)
		return -EINVAL;
	if (size < 2 || !(data[1] & 7))
		return -EINVAL;
	if ((data[0] & 1) || (data[1] & 0xf8)) /* nuh_layer_id */
		return -EOPNOTSUPP;
	type = (data[0] >> 1) & 0x3f;
	if (type == 33)
		return vcp_hevc_sps_guard(data + 2, size - 2);
	return 0;
}

/* V4L2 H264/HEVC OUTPUT carries Annex B, with complete NAL units. Inspect
 * every NAL: an AU may contain several SPSs or an AUD/SEI before the SPS.
 * No sequence cache is used, so queueing ahead and DRC cannot bypass checks.
 */
/* VP8 frame tag and key-frame geometry precede the bool-coded partitions. */
static inline int vcp_vp8_guard(const u8 *data, size_t size)
{
	u32 tag, width, height;
	size_t header;

	if (!data || size < 3)
		return -EINVAL;
	tag = data[0] | (u32)data[1] << 8 | (u32)data[2] << 16;
	if (((tag >> 1) & 7) > 3)
		return -EOPNOTSUPP;
	header = tag & 1 ? 3 : 10;
	if (size < header || !(tag >> 5) || (tag >> 5) > size - header)
		return -EINVAL;
	if (!(tag & 1)) {
		if (data[3] != 0x9d || data[4] != 0x01 || data[5] != 0x2a)
			return -EINVAL;
		width = (data[6] | (u32)data[7] << 8) & 0x3fff;
		height = (data[8] | (u32)data[9] << 8) & 0x3fff;
		if (!width || !height)
			return -EINVAL;
		if (width > 4096 || height > 2176)
			return -EOPNOTSUPP;
	}
	return 0;
}

static inline int vcp_vdec_bitstream_guard(u32 fourcc, const u8 *data, size_t size)
{
	size_t i, start = 0, zeros = 0;
	bool found = false;
	int ret;

	if (fourcc == V4L2_PIX_FMT_VP8)
		return vcp_vp8_guard(data, size);
	if (fourcc == V4L2_PIX_FMT_VP9)
		return vcp_vp9_guard(data, size);
	if (!data)
		return -EINVAL;
	for (i = 0; i < size; i++) {
		if (!data[i]) {
			zeros++;
			continue;
		}
		if (data[i] == 1 && zeros >= 2) {
			if (found) {
				ret = vcp_vdec_nal_guard(fourcc, data + start,
						 i - zeros - start);
				if (ret)
					return ret;
			}
			start = i + 1;
			found = true;
		} else if (!found) {
			return -EINVAL;
		}
		zeros = 0;
	}
	if (!found)
		return -EINVAL;
	return vcp_vdec_nal_guard(fourcc, data + start, size - start);
}

#endif
