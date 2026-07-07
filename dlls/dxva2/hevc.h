/*
 * DXVA2 HEVC picture/slice parameter translation to VA-API
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

#ifndef __DXVA2_HEVC_H
#define __DXVA2_HEVC_H

#ifdef HAVE_LIBVA

void dxva2_hevc_translate_pic_params(const DXVA_PicParams_HEVC *src, VAPictureParameterBufferHEVC *dst,
        UINT bitdepth_luma, UINT bitdepth_chroma, const VASurfaceID *surfaces, UINT surface_count);

void dxva2_hevc_translate_iq_matrix(const DXVA_Qmatrix_HEVC *src, VAIQMatrixBufferHEVC *dst);

BOOL dxva2_hevc_translate_slice(const DXVA_PicParams_HEVC *pic, const BYTE *nal_data, UINT nal_size,
        VASliceParameterBufferHEVC *dst, UINT slice_data_offset);

#endif /* HAVE_LIBVA */

#endif /* __DXVA2_HEVC_H */
