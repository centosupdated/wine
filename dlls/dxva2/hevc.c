/*
 * DXVA2 HEVC picture/slice parameter translation to VA-API
 *
 * Translates the DXVA_PicParams_HEVC / DXVA_Qmatrix_HEVC / DXVA_Slice_HEVC_Short
 * structures an app fills in (matching the exact wire format real DXVA2 HEVC
 * decoders use, taken straight from the public DXVA HEVC spec) into their VA-API
 * equivalents (VAPictureParameterBufferHEVC / VAIQMatrixBufferHEVC /
 * VASliceParameterBufferHEVC).
 *
 * The picture-parameter and IQ-matrix structures correspond field for field (both
 * ultimately mirror the same H.265 SPS/PPS syntax elements), so that half is a
 * mechanical copy. The harder part is slice parameters: DXVA's "short slice format"
 * (what we advertised via ConfigBitstreamRaw=2 in GetDecoderConfigurations) only
 * gives the NAL unit's location/size in the bitstream and expects the decoder to
 * parse the slice segment header itself - VA-API's public API has no equivalent
 * "let the driver parse it" mode for HEVC, so we parse slice_segment_header() by
 * hand here (H.265 7.3.6.1) to fill in VASliceParameterBufferHEVC.
 *
 * Copyright 2026 Valerian Mayega
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBVA
#include <va/va.h>
#include <va/va_dec_hevc.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "dxva.h"

#include "wine/debug.h"
#include "hevc.h"

#ifdef HAVE_LIBVA

WINE_DEFAULT_DEBUG_CHANNEL(dxva2);

#define DXVA_PICENTRY_INVALID 0xff

/* ---- Exp-Golomb bit reader, operating on an already emulation-prevention-stripped buffer ---- */

struct bitreader
{
    const BYTE *data;
    UINT size;      /* bytes */
    UINT byte_pos;
    UINT bit_pos;   /* 0-7, MSB first */
};

static void br_init(struct bitreader *br, const BYTE *data, UINT size)
{
    br->data = data;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 0;
}

static UINT32 br_bit(struct bitreader *br)
{
    UINT32 bit;

    if (br->byte_pos >= br->size) return 0;
    bit = (br->data[br->byte_pos] >> (7 - br->bit_pos)) & 1;
    if (++br->bit_pos == 8)
    {
        br->bit_pos = 0;
        br->byte_pos++;
    }
    return bit;
}

static UINT32 br_bits(struct bitreader *br, UINT n)
{
    UINT32 v = 0;
    while (n--) v = (v << 1) | br_bit(br);
    return v;
}

static UINT32 br_ue(struct bitreader *br)
{
    UINT zeros = 0;
    while (br->byte_pos < br->size && !br_bit(br)) zeros++;
    if (!zeros) return 0;
    return (1u << zeros) - 1 + br_bits(br, zeros);
}

static INT32 br_se(struct bitreader *br)
{
    UINT32 v = br_ue(br);
    return (v & 1) ? (INT32)((v + 1) / 2) : -(INT32)(v / 2);
}

static UINT ceil_log2(UINT v)
{
    UINT r = 0;
    while ((1u << r) < v) r++;
    return r;
}

/* Strip emulation prevention bytes (00 00 03 -> 00 00) into a fresh heap buffer;
 * the input still has them (DXVA hands us the raw bitstream), but Exp-Golomb
 * parsing must operate on the de-escaped stream. Caller frees the result. */
static BYTE *strip_emulation_prevention(const BYTE *data, UINT size, UINT *out_size)
{
    BYTE *out;
    UINT i, o = 0, zeros = 0;

    if (!(out = malloc(size))) return NULL;

    for (i = 0; i < size; i++)
    {
        if (zeros >= 2 && data[i] == 0x03 && i + 1 < size && data[i + 1] <= 0x03)
        {
            zeros = 0;
            continue;
        }
        out[o++] = data[i];
        zeros = data[i] == 0 ? zeros + 1 : 0;
    }

    *out_size = o;
    return out;
}

/* ---- Picture parameters + IQ matrix: near-mechanical field-for-field copies ---- */

void dxva2_hevc_translate_pic_params(const DXVA_PicParams_HEVC *src, VAPictureParameterBufferHEVC *dst,
        UINT bitdepth_luma, UINT bitdepth_chroma, const VASurfaceID *surfaces, UINT surface_count)
{
    unsigned int i, before = 0, after = 0, lt = 0;

    memset(dst, 0, sizeof(*dst));

    dst->CurrPic.picture_id = src->CurrPic.Index7Bits < surface_count ?
            surfaces[src->CurrPic.Index7Bits] : VA_INVALID_SURFACE;
    dst->CurrPic.pic_order_cnt = src->CurrPicOrderCntVal;
    dst->CurrPic.flags = 0;

    for (i = 0; i < 15; i++)
    {
        BYTE entry = src->RefPicList[i].bPicEntry;

        if (entry == DXVA_PICENTRY_INVALID || src->RefPicList[i].Index7Bits >= surface_count)
        {
            dst->ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
            dst->ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
            continue;
        }

        dst->ReferenceFrames[i].picture_id = surfaces[src->RefPicList[i].Index7Bits];
        dst->ReferenceFrames[i].pic_order_cnt = src->PicOrderCntValList[i];
        dst->ReferenceFrames[i].flags = src->RefPicList[i].AssociatedFlag ? VA_PICTURE_HEVC_LONG_TERM_REFERENCE : 0;
    }

    /* DXVA already resolves the reference-picture-set categorization at the picture
     * level (RefPicSetStCurrBefore/After/LtCurr, arrays of indices into RefPicList[],
     * DXVA_PICENTRY_INVALID-padded); VA-API wants the same information as per-entry
     * flags on ReferenceFrames[] instead. */
    for (i = 0; i < 8 && src->RefPicSetStCurrBefore[i] != DXVA_PICENTRY_INVALID; i++)
    {
        dst->ReferenceFrames[src->RefPicSetStCurrBefore[i]].flags |= VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
        before++;
    }
    for (i = 0; i < 8 && src->RefPicSetStCurrAfter[i] != DXVA_PICENTRY_INVALID; i++)
    {
        dst->ReferenceFrames[src->RefPicSetStCurrAfter[i]].flags |= VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
        after++;
    }
    for (i = 0; i < 8 && src->RefPicSetLtCurr[i] != DXVA_PICENTRY_INVALID; i++)
    {
        dst->ReferenceFrames[src->RefPicSetLtCurr[i]].flags |= VA_PICTURE_HEVC_RPS_LT_CURR;
        lt++;
    }

    dst->pic_width_in_luma_samples = src->PicWidthInMinCbsY <<
            (src->log2_min_luma_coding_block_size_minus3 + 3);
    dst->pic_height_in_luma_samples = src->PicHeightInMinCbsY <<
            (src->log2_min_luma_coding_block_size_minus3 + 3);

    dst->pic_fields.bits.chroma_format_idc = src->chroma_format_idc;
    dst->pic_fields.bits.separate_colour_plane_flag = src->separate_colour_plane_flag;
    dst->pic_fields.bits.pcm_enabled_flag = src->pcm_enabled_flag;
    dst->pic_fields.bits.scaling_list_enabled_flag = src->scaling_list_enabled_flag;
    dst->pic_fields.bits.transform_skip_enabled_flag = src->transform_skip_enabled_flag;
    dst->pic_fields.bits.amp_enabled_flag = src->amp_enabled_flag;
    dst->pic_fields.bits.strong_intra_smoothing_enabled_flag = src->strong_intra_smoothing_enabled_flag;
    dst->pic_fields.bits.sign_data_hiding_enabled_flag = src->sign_data_hiding_enabled_flag;
    dst->pic_fields.bits.constrained_intra_pred_flag = src->constrained_intra_pred_flag;
    dst->pic_fields.bits.cu_qp_delta_enabled_flag = src->cu_qp_delta_enabled_flag;
    dst->pic_fields.bits.weighted_pred_flag = src->weighted_pred_flag;
    dst->pic_fields.bits.weighted_bipred_flag = src->weighted_bipred_flag;
    dst->pic_fields.bits.transquant_bypass_enabled_flag = src->transquant_bypass_enabled_flag;
    dst->pic_fields.bits.tiles_enabled_flag = src->tiles_enabled_flag;
    dst->pic_fields.bits.entropy_coding_sync_enabled_flag = src->entropy_coding_sync_enabled_flag;
    dst->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = src->pps_loop_filter_across_slices_enabled_flag;
    dst->pic_fields.bits.loop_filter_across_tiles_enabled_flag = src->loop_filter_across_tiles_enabled_flag;
    dst->pic_fields.bits.pcm_loop_filter_disabled_flag = src->pcm_loop_filter_disabled_flag;
    dst->pic_fields.bits.NoPicReorderingFlag = src->NoPicReorderingFlag;
    dst->pic_fields.bits.NoBiPredFlag = src->NoBiPredFlag;

    dst->sps_max_dec_pic_buffering_minus1 = src->sps_max_dec_pic_buffering_minus1;
    dst->bit_depth_luma_minus8 = bitdepth_luma - 8;
    dst->bit_depth_chroma_minus8 = bitdepth_chroma - 8;
    dst->pcm_sample_bit_depth_luma_minus1 = src->pcm_sample_bit_depth_luma_minus1;
    dst->pcm_sample_bit_depth_chroma_minus1 = src->pcm_sample_bit_depth_chroma_minus1;
    dst->log2_min_luma_coding_block_size_minus3 = src->log2_min_luma_coding_block_size_minus3;
    dst->log2_diff_max_min_luma_coding_block_size = src->log2_diff_max_min_luma_coding_block_size;
    dst->log2_min_transform_block_size_minus2 = src->log2_min_transform_block_size_minus2;
    dst->log2_diff_max_min_transform_block_size = src->log2_diff_max_min_transform_block_size;
    dst->log2_min_pcm_luma_coding_block_size_minus3 = src->log2_min_pcm_luma_coding_block_size_minus3;
    dst->log2_diff_max_min_pcm_luma_coding_block_size = src->log2_diff_max_min_pcm_luma_coding_block_size;
    dst->max_transform_hierarchy_depth_intra = src->max_transform_hierarchy_depth_intra;
    dst->max_transform_hierarchy_depth_inter = src->max_transform_hierarchy_depth_inter;
    dst->init_qp_minus26 = src->init_qp_minus26;
    dst->diff_cu_qp_delta_depth = src->diff_cu_qp_delta_depth;
    dst->pps_cb_qp_offset = src->pps_cb_qp_offset;
    dst->pps_cr_qp_offset = src->pps_cr_qp_offset;
    dst->log2_parallel_merge_level_minus2 = src->log2_parallel_merge_level_minus2;
    dst->num_tile_columns_minus1 = src->num_tile_columns_minus1;
    dst->num_tile_rows_minus1 = src->num_tile_rows_minus1;
    memcpy(dst->column_width_minus1, src->column_width_minus1, sizeof(dst->column_width_minus1));
    memcpy(dst->row_height_minus1, src->row_height_minus1, sizeof(dst->row_height_minus1));

    dst->slice_parsing_fields.bits.lists_modification_present_flag = src->lists_modification_present_flag;
    dst->slice_parsing_fields.bits.long_term_ref_pics_present_flag = src->long_term_ref_pics_present_flag;
    dst->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = src->sps_temporal_mvp_enabled_flag;
    dst->slice_parsing_fields.bits.cabac_init_present_flag = src->cabac_init_present_flag;
    dst->slice_parsing_fields.bits.output_flag_present_flag = src->output_flag_present_flag;
    dst->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag = src->dependent_slice_segments_enabled_flag;
    dst->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag = src->pps_slice_chroma_qp_offsets_present_flag;
    dst->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = src->sample_adaptive_offset_enabled_flag;
    dst->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag = src->deblocking_filter_override_enabled_flag;
    dst->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag = src->pps_deblocking_filter_disabled_flag;
    dst->slice_parsing_fields.bits.slice_segment_header_extension_present_flag = src->slice_segment_header_extension_present_flag;
    dst->slice_parsing_fields.bits.RapPicFlag = src->IrapPicFlag;
    dst->slice_parsing_fields.bits.IdrPicFlag = src->IdrPicFlag;
    dst->slice_parsing_fields.bits.IntraPicFlag = src->IntraPicFlag;

    dst->log2_max_pic_order_cnt_lsb_minus4 = src->log2_max_pic_order_cnt_lsb_minus4;
    dst->num_short_term_ref_pic_sets = src->num_short_term_ref_pic_sets;
    dst->num_long_term_ref_pic_sps = src->num_long_term_ref_pics_sps;
    dst->num_ref_idx_l0_default_active_minus1 = src->num_ref_idx_l0_default_active_minus1;
    dst->num_ref_idx_l1_default_active_minus1 = src->num_ref_idx_l1_default_active_minus1;
    dst->pps_beta_offset_div2 = src->pps_beta_offset_div2;
    dst->pps_tc_offset_div2 = src->pps_tc_offset_div2;
    dst->num_extra_slice_header_bits = src->num_extra_slice_header_bits;
    dst->st_rps_bits = src->wNumBitsForShortTermRPSInSlice;

    TRACE("HEVC pic params: %ux%u, %u/%u/%u ref pics before/after/lt.\n",
            dst->pic_width_in_luma_samples, dst->pic_height_in_luma_samples, before, after, lt);
}

void dxva2_hevc_translate_iq_matrix(const DXVA_Qmatrix_HEVC *src, VAIQMatrixBufferHEVC *dst)
{
    memset(dst, 0, sizeof(*dst));
    memcpy(dst->ScalingList4x4, src->ucScalingLists0, sizeof(dst->ScalingList4x4));
    memcpy(dst->ScalingList8x8, src->ucScalingLists1, sizeof(dst->ScalingList8x8));
    memcpy(dst->ScalingList16x16, src->ucScalingLists2, sizeof(dst->ScalingList16x16));
    memcpy(dst->ScalingList32x32, src->ucScalingLists3, sizeof(dst->ScalingList32x32));
    memcpy(dst->ScalingListDC16x16, src->ucScalingListDCCoefSizeID2, sizeof(dst->ScalingListDC16x16));
    memcpy(dst->ScalingListDC32x32, src->ucScalingListDCCoefSizeID3, sizeof(dst->ScalingListDC32x32));
}

/* ---- Slice header parsing: DXVA's "short format" only gives us NAL location/size,
 * so we parse slice_segment_header() ourselves (H.265 7.3.6.1) to get the fields
 * VA-API's "long format" VASliceParameterBufferHEVC needs. The picture-level fields
 * (RPS categorization, SPS/PPS flags) are already resolved for us in DXVA_PicParams_HEVC;
 * what's left to parse is genuinely per-slice: type, overrides, ref list modification,
 * weighted prediction table, deblocking overrides, QP deltas. ---- */

struct ref_pic_lists
{
    BYTE list0[15], list0_count;
    BYTE list1[15], list1_count;
};

/* H.265 8.3.4: default (pre-modification) RefPicList construction from the picture-level
 * RPS categorization DXVA already resolved for us. */
static void build_default_ref_pic_lists(const DXVA_PicParams_HEVC *pic, struct ref_pic_lists *lists)
{
    BYTE before[8], after[8], lt[8];
    UINT nbefore = 0, nafter = 0, nlt = 0, i, total;

    for (i = 0; i < 8 && pic->RefPicSetStCurrBefore[i] != DXVA_PICENTRY_INVALID; i++) before[nbefore++] = pic->RefPicSetStCurrBefore[i];
    for (i = 0; i < 8 && pic->RefPicSetStCurrAfter[i] != DXVA_PICENTRY_INVALID; i++) after[nafter++] = pic->RefPicSetStCurrAfter[i];
    for (i = 0; i < 8 && pic->RefPicSetLtCurr[i] != DXVA_PICENTRY_INVALID; i++) lt[nlt++] = pic->RefPicSetLtCurr[i];

    total = nbefore + nafter + nlt;
    lists->list0_count = 0;
    lists->list1_count = 0;
    if (!total) return;

    /* List0 candidate order: StCurrBefore, StCurrAfter, LtCurr - repeated (wrapped) up to
     * NumPicTotalCurr entries; the caller truncates to num_ref_idx_l0_active_minus1+1. */
    for (i = 0; i < 15; i++)
    {
        BYTE v;
        UINT idx = i % total;
        if (idx < nbefore) v = before[idx];
        else if (idx < nbefore + nafter) v = after[idx - nbefore];
        else v = lt[idx - nbefore - nafter];
        lists->list0[i] = v;
    }
    lists->list0_count = 15;

    /* List1 candidate order: StCurrAfter, StCurrBefore, LtCurr. */
    for (i = 0; i < 15; i++)
    {
        BYTE v;
        UINT idx = i % total;
        if (idx < nafter) v = after[idx];
        else if (idx < nafter + nbefore) v = before[idx - nafter];
        else v = lt[idx - nafter - nbefore];
        lists->list1[i] = v;
    }
    lists->list1_count = 15;
}

BOOL dxva2_hevc_translate_slice(const DXVA_PicParams_HEVC *pic, const BYTE *nal_data, UINT nal_size,
        VASliceParameterBufferHEVC *dst, UINT slice_data_offset)
{
    struct bitreader br;
    struct ref_pic_lists def_lists;
    BYTE *stripped;
    UINT stripped_size, i;
    UINT nal_unit_type;
    BOOL first_slice_segment_in_pic_flag, dependent_slice_segment_flag = FALSE;
    BOOL slice_sao_luma_flag = FALSE, slice_sao_chroma_flag = FALSE;
    BOOL slice_deblocking_filter_disabled_flag = pic->pps_deblocking_filter_disabled_flag;
    UINT slice_type;
    UINT pic_size_in_ctbs;
    UINT ctb_log2_size;
    UINT num_pic_total_curr;

    memset(dst, 0, sizeof(*dst));
    dst->collocated_ref_idx = 0xff; /* "invalid" when slice_temporal_mvp is off */

    if (nal_size < 3) return FALSE;
    nal_unit_type = (nal_data[0] >> 1) & 0x3f;

    if (!(stripped = strip_emulation_prevention(nal_data, nal_size, &stripped_size)))
        return FALSE;

    br_init(&br, stripped, stripped_size);
    br_bits(&br, 16); /* NAL unit header, already accounted for via nal_unit_type above */

    first_slice_segment_in_pic_flag = br_bit(&br);
    if (nal_unit_type >= 16 && nal_unit_type <= 23) br_bit(&br); /* no_output_of_prior_pics_flag */
    br_ue(&br); /* slice_pic_parameter_set_id */

    ctb_log2_size = pic->log2_min_luma_coding_block_size_minus3 + 3 + pic->log2_diff_max_min_luma_coding_block_size;
    pic_size_in_ctbs = ((pic->PicWidthInMinCbsY << (pic->log2_min_luma_coding_block_size_minus3 + 3)) + (1u << ctb_log2_size) - 1) >> ctb_log2_size;
    pic_size_in_ctbs *= ((pic->PicHeightInMinCbsY << (pic->log2_min_luma_coding_block_size_minus3 + 3)) + (1u << ctb_log2_size) - 1) >> ctb_log2_size;

    if (!first_slice_segment_in_pic_flag)
    {
        if (pic->dependent_slice_segments_enabled_flag) dependent_slice_segment_flag = br_bit(&br);
        dst->slice_segment_address = br_bits(&br, ceil_log2(pic_size_in_ctbs ? pic_size_in_ctbs : 1));
    }

    dst->LongSliceFlags.fields.dependent_slice_segment_flag = dependent_slice_segment_flag;

    build_default_ref_pic_lists(pic, &def_lists);
    num_pic_total_curr = 0;
    for (i = 0; i < 8 && pic->RefPicSetStCurrBefore[i] != DXVA_PICENTRY_INVALID; i++) num_pic_total_curr++;
    for (i = 0; i < 8 && pic->RefPicSetStCurrAfter[i] != DXVA_PICENTRY_INVALID; i++) num_pic_total_curr++;
    for (i = 0; i < 8 && pic->RefPicSetLtCurr[i] != DXVA_PICENTRY_INVALID; i++) num_pic_total_curr++;

    slice_type = 2; /* I, default/fallback if we bail early */

    if (!dependent_slice_segment_flag)
    {
        for (i = 0; i < pic->num_extra_slice_header_bits; i++) br_bit(&br);
        slice_type = br_ue(&br);
        if (pic->output_flag_present_flag) br_bit(&br); /* pic_output_flag */
        if (pic->separate_colour_plane_flag) br_bits(&br, 2); /* colour_plane_id */

        if (!pic->IdrPicFlag)
        {
            br_bits(&br, pic->log2_max_pic_order_cnt_lsb_minus4 + 4); /* slice_pic_order_cnt_lsb - already have POC from pic params */
            if (!br_bit(&br)) /* short_term_ref_pic_set_sps_flag == 0: RPS coded inline */
            {
                /* Full short_term_ref_pic_set() parsing is involved and, when present, DXVA
                 * still gives us the resolved RefPicSetStCurrBefore/After/LtCurr categorization at the
                 * picture level regardless - so we only need to consume the right number of
                 * bits here to stay in sync with the rest of the header, which DXVA gives us
                 * directly via wNumBitsForShortTermRPSInSlice. */
                UINT rps_bits = pic->wNumBitsForShortTermRPSInSlice;
                while (rps_bits > 32)
                {
                    br_bits(&br, 32);
                    rps_bits -= 32;
                }
                br_bits(&br, rps_bits);
            }
            else if (pic->num_short_term_ref_pic_sets > 1)
            {
                br_bits(&br, ceil_log2(pic->num_short_term_ref_pic_sets));
            }
            if (pic->long_term_ref_pics_present_flag)
            {
                /* DXVA already resolves the final LT set for us at the picture level;
                 * we only parse to keep the bit position in sync (7.3.6.1). */
                UINT num_lt_sps = 0, num_lt_pics, total_lt;

                if (pic->num_long_term_ref_pics_sps > 0) num_lt_sps = br_ue(&br);
                num_lt_pics = br_ue(&br);
                total_lt = num_lt_sps + num_lt_pics;
                for (i = 0; i < total_lt; i++)
                {
                    if (i < num_lt_sps)
                    {
                        if (pic->num_long_term_ref_pics_sps > 1)
                            br_bits(&br, ceil_log2(pic->num_long_term_ref_pics_sps)); /* lt_idx_sps */
                    }
                    else
                    {
                        br_bits(&br, pic->log2_max_pic_order_cnt_lsb_minus4 + 4); /* poc_lsb_lt */
                        br_bit(&br); /* used_by_curr_pic_lt_flag */
                    }
                    if (br_bit(&br)) /* delta_poc_msb_present_flag */
                        br_ue(&br); /* delta_poc_msb_cycle_lt */
                }
            }
            if (pic->sps_temporal_mvp_enabled_flag)
                dst->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = br_bit(&br);
        }

        if (pic->sample_adaptive_offset_enabled_flag)
        {
            slice_sao_luma_flag = br_bit(&br);
            if (pic->chroma_format_idc) slice_sao_chroma_flag = br_bit(&br);
        }

        dst->num_ref_idx_l0_active_minus1 = pic->num_ref_idx_l0_default_active_minus1;
        dst->num_ref_idx_l1_active_minus1 = pic->num_ref_idx_l1_default_active_minus1;

        if (slice_type == 0 /* B */ || slice_type == 1 /* P */)
        {
            BOOL override_flag = br_bit(&br);
            if (override_flag)
            {
                dst->num_ref_idx_l0_active_minus1 = br_ue(&br);
                if (slice_type == 0) dst->num_ref_idx_l1_active_minus1 = br_ue(&br);
            }

            if (pic->lists_modification_present_flag && num_pic_total_curr > 1)
            {
                UINT bits = ceil_log2(num_pic_total_curr);
                if (br_bit(&br)) /* ref_pic_list_modification_flag_l0 */
                    for (i = 0; i <= dst->num_ref_idx_l0_active_minus1; i++) def_lists.list0[i] = br_bits(&br, bits);
                if (slice_type == 0 && br_bit(&br)) /* ref_pic_list_modification_flag_l1 */
                    for (i = 0; i <= dst->num_ref_idx_l1_active_minus1; i++) def_lists.list1[i] = br_bits(&br, bits);
            }

            if (slice_type == 0) dst->LongSliceFlags.fields.mvd_l1_zero_flag = br_bit(&br);
            if (pic->cabac_init_present_flag) dst->LongSliceFlags.fields.cabac_init_flag = br_bit(&br);

            dst->collocated_ref_idx = 0xff;
            if (dst->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag)
            {
                BOOL collocated_from_l0 = TRUE;
                if (slice_type == 0) collocated_from_l0 = br_bit(&br);
                dst->LongSliceFlags.fields.collocated_from_l0_flag = collocated_from_l0;

                if ((collocated_from_l0 && dst->num_ref_idx_l0_active_minus1 > 0) ||
                        (!collocated_from_l0 && dst->num_ref_idx_l1_active_minus1 > 0))
                    dst->collocated_ref_idx = br_ue(&br);
                else
                    dst->collocated_ref_idx = 0;
            }

            if ((pic->weighted_pred_flag && slice_type == 1) || (pic->weighted_bipred_flag && slice_type == 0))
            {
                UINT c_log2_denom;

                dst->luma_log2_weight_denom = br_ue(&br);
                if (pic->chroma_format_idc)
                    dst->delta_chroma_log2_weight_denom = br_se(&br);
                c_log2_denom = dst->luma_log2_weight_denom + dst->delta_chroma_log2_weight_denom;

                for (i = 0; i <= dst->num_ref_idx_l0_active_minus1; i++)
                {
                    BOOL luma_flag = br_bit(&br);
                    BOOL chroma_flag = pic->chroma_format_idc ? br_bit(&br) : FALSE;
                    if (luma_flag)
                    {
                        dst->delta_luma_weight_l0[i] = br_se(&br);
                        dst->luma_offset_l0[i] = br_se(&br);
                    }
                    if (chroma_flag)
                    {
                        int j;
                        for (j = 0; j < 2; j++)
                        {
                            INT32 delta_weight = br_se(&br);
                            INT32 delta_offset = br_se(&br);
                            dst->delta_chroma_weight_l0[i][j] = delta_weight;
                            dst->ChromaOffsetL0[i][j] = delta_offset;
                        }
                        (void)c_log2_denom;
                    }
                }
                if (slice_type == 0)
                {
                    for (i = 0; i <= dst->num_ref_idx_l1_active_minus1; i++)
                    {
                        BOOL luma_flag = br_bit(&br);
                        BOOL chroma_flag = pic->chroma_format_idc ? br_bit(&br) : FALSE;
                        if (luma_flag)
                        {
                            dst->delta_luma_weight_l1[i] = br_se(&br);
                            dst->luma_offset_l1[i] = br_se(&br);
                        }
                        if (chroma_flag)
                        {
                            int j;
                            for (j = 0; j < 2; j++)
                            {
                                dst->delta_chroma_weight_l1[i][j] = br_se(&br);
                                dst->ChromaOffsetL1[i][j] = br_se(&br);
                            }
                        }
                    }
                }
            }

            dst->five_minus_max_num_merge_cand = br_ue(&br);
        }

        dst->slice_qp_delta = br_se(&br);
        if (pic->pps_slice_chroma_qp_offsets_present_flag)
        {
            dst->slice_cb_qp_offset = br_se(&br);
            dst->slice_cr_qp_offset = br_se(&br);
        }

        if (pic->deblocking_filter_override_enabled_flag && br_bit(&br)) /* deblocking_filter_override_flag */
        {
            slice_deblocking_filter_disabled_flag = br_bit(&br);
            if (!slice_deblocking_filter_disabled_flag)
            {
                dst->slice_beta_offset_div2 = br_se(&br);
                dst->slice_tc_offset_div2 = br_se(&br);
            }
        }

        if (pic->pps_loop_filter_across_slices_enabled_flag &&
                (slice_sao_luma_flag || slice_sao_chroma_flag || !slice_deblocking_filter_disabled_flag))
            dst->LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = br_bit(&br);
    }

    /* Remaining slice_segment_header() syntax, parsed for every slice segment
     * (dependent ones included): entry point offsets and the header extension. */
    if (pic->tiles_enabled_flag || pic->entropy_coding_sync_enabled_flag)
    {
        UINT num_entry_point_offsets = br_ue(&br);
        if (num_entry_point_offsets > 0)
        {
            UINT offset_len = br_ue(&br) + 1; /* offset_len_minus1 */
            for (i = 0; i < num_entry_point_offsets; i++)
                br_bits(&br, offset_len);
        }
    }
    if (pic->slice_segment_header_extension_present_flag)
    {
        UINT ext_len = br_ue(&br); /* slice_segment_header_extension_length */
        for (i = 0; i < ext_len; i++)
            br_bits(&br, 8);
    }

    /* slice_segment_header() ends with byte_alignment(); slice_data() starts on
     * the next byte boundary. VA-API needs this offset (relative to and
     * including the NAL unit header, counted after emulation prevention byte
     * removal - exactly our bit reader's position in the stripped stream) to
     * know where entropy-coded data begins, since with DXVA's short format the
     * driver never sees a parsed slice header. */
    dst->slice_data_byte_offset = br.byte_pos + (br.bit_pos ? 1 : 0);

    free(stripped);

    dst->LongSliceFlags.fields.LastSliceOfPic = 0; /* set by caller for the actual last slice */
    dst->LongSliceFlags.fields.slice_type = slice_type;
    dst->LongSliceFlags.fields.slice_sao_luma_flag = slice_sao_luma_flag;
    dst->LongSliceFlags.fields.slice_sao_chroma_flag = slice_sao_chroma_flag;
    dst->LongSliceFlags.fields.slice_deblocking_filter_disabled_flag = slice_deblocking_filter_disabled_flag;

    for (i = 0; i < 15; i++)
    {
        dst->RefPicList[0][i] = i < def_lists.list0_count ? def_lists.list0[i] : 0xff;
        dst->RefPicList[1][i] = i < def_lists.list1_count ? def_lists.list1[i] : 0xff;
    }

    dst->slice_data_offset = slice_data_offset;
    dst->slice_data_size = nal_size;
    dst->slice_data_flag = 0; /* VA_SLICE_DATA_FLAG_ALL: buffer holds the complete slice */

    return TRUE;
}

#endif /* HAVE_LIBVA */
