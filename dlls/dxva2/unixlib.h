/*
 * Unix interface for dxva2 VA-API hardware decode capability queries
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

#ifndef __DXVA2_UNIXLIB_H
#define __DXVA2_UNIXLIB_H

#include "windef.h"
#include "wine/unixlib.h"

#define DXVA2_MAX_DECODER_PROFILES 16

struct dxva2_decoder_profile
{
    GUID guid;
    UINT bitdepth;
};

struct query_decoder_profiles_params
{
    struct dxva2_decoder_profile *profiles; /* [out] caller-allocated array */
    UINT capacity;                          /* [in] number of entries `profiles` can hold */
    UINT count;                             /* [out] number of entries filled in */
};

struct decoder_create_params
{
    GUID guid;             /* [in] DXVA2 decoder mode GUID, e.g. DXVA2_ModeHEVC_VLD_Main */
    UINT width;             /* [in] */
    UINT height;            /* [in] */
    UINT surface_count;     /* [in] number of decode surfaces to allocate (matches D3D9 surface array size) */
    UINT64 context;          /* [out] opaque handle for later decoder_destroy/decoder_decode_frame calls */
};

struct decoder_destroy_params
{
    UINT64 context; /* [in] */
};

/* One decode_frame call handles exactly one compressed picture: a picture-parameters
 * buffer, an optional inverse-quantization-matrix buffer, and one or more slices
 * (short format: NAL location/size per slice, DXVA_Slice_HEVC_Short array) sharing a
 * single bitstream buffer. Output is written directly into a caller-allocated NV12/P010
 * buffer matching the target D3D9 surface's locked memory, avoiding an extra copy on
 * the unix side (the PE side still does one copy: VA's derived image -> the locked D3D9
 * surface, since VA image memory can't be the D3D9 surface's backing store directly).
 */
struct decoder_decode_params
{
    UINT64 context;              /* [in] */
    UINT target_surface_index;    /* [in] index into the surface array from decoder_create, selects the DPB slot this frame decodes into */

    const void *pic_params;       /* [in] raw DXVA_PicParams_HEVC bytes, as filled by the app */
    UINT pic_params_size;         /* [in] */

    const void *qmatrix;          /* [in] raw DXVA_Qmatrix_HEVC bytes, or NULL if scaling_list_enabled_flag is off */
    UINT qmatrix_size;            /* [in] */

    const void *slice_control;    /* [in] array of DXVA_Slice_HEVC_Short */
    UINT slice_count;             /* [in] number of entries in slice_control */

    const void *bitstream;        /* [in] raw compressed NAL data for all slices in this frame */
    UINT bitstream_size;          /* [in] */

    void *output;                  /* [out] caller-allocated buffer, NV12 or P010, tightly matching output_stride/output_height */
    UINT output_stride;           /* [in] bytes per row of the luma plane (chroma plane assumed same stride, half height) */
    UINT output_height;           /* [in] luma plane height in pixels */
};

enum unix_funcs
{
    unix_query_decoder_profiles,
    unix_decoder_create,
    unix_decoder_destroy,
    unix_decoder_decode_frame,
    unix_funcs_count,
};

#define DXVA2_CALL( func, params ) WINE_UNIX_CALL( unix_ ## func, params )

#endif /* __DXVA2_UNIXLIB_H */
