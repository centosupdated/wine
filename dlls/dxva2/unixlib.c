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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef HAVE_LIBVA
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_dec_hevc.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "dxva.h"

#include "wine/debug.h"
#include "unixlib.h"
#include "hevc.h"

#ifdef HAVE_LIBVA

WINE_DEFAULT_DEBUG_CHANNEL(dxva2);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif


/* DXVA2 decoder mode GUIDs, mirrors include/dxva2api.idl. Defined locally
 * since this file builds against the host libc/toolchain, not Wine's PE
 * headers, so DEFINE_GUID isn't available here. */
static const GUID guid_h264_vld_nofgt =
    {0x1b81be68, 0xa0c7, 0x11d3, {0xb9,0x84,0x00,0xc0,0x4f,0x2e,0x73,0xc5}};
static const GUID guid_hevc_vld_main =
    {0x5b11d51b, 0x2f4c, 0x4452, {0xbc,0xc3,0x09,0xf2,0xa1,0x16,0x0c,0xc0}};
static const GUID guid_hevc_vld_main10 =
    {0x107af0e0, 0xef1a, 0x4d19, {0xab,0xa8,0x67,0xa1,0x63,0x07,0x3d,0x13}};

static VADisplay va_open( int *fd )
{
    static const char *devices[] = { "/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/dri/card0" };
    VADisplay display;
    int major, minor;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(devices); i++)
    {
        if ((*fd = open( devices[i], O_RDWR )) < 0) continue;

        if (!(display = vaGetDisplayDRM( *fd )))
        {
            close( *fd );
            *fd = -1;
            continue;
        }

        if (vaInitialize( display, &major, &minor ) == VA_STATUS_SUCCESS)
        {
            TRACE( "Opened VA-API display on %s, version %d.%d.\n", devices[i], major, minor );
            return display;
        }

        close( *fd );
        *fd = -1;
    }

    WARN( "Could not open a working VA-API display.\n" );
    return NULL;
}

static NTSTATUS query_decoder_profiles( void *args )
{
    struct query_decoder_profiles_params *params = args;
    VADisplay display;
    VAProfile *profiles;
    int fd = -1, i, num_profiles;
    UINT count = 0;

    params->count = 0;

    if (!(display = va_open( &fd )))
        return STATUS_SUCCESS;

    num_profiles = vaMaxNumProfiles( display );
    if (num_profiles <= 0 || !(profiles = malloc( num_profiles * sizeof(*profiles) )))
    {
        vaTerminate( display );
        close( fd );
        return STATUS_SUCCESS;
    }

    if (vaQueryConfigProfiles( display, profiles, &num_profiles ) == VA_STATUS_SUCCESS)
    {
        for (i = 0; i < num_profiles && count < params->capacity; i++)
        {
            VAConfigAttrib attrib;
            const GUID *guid = NULL;
            UINT bitdepth = 8;

            switch (profiles[i])
            {
            case VAProfileH264ConstrainedBaseline:
            case VAProfileH264Main:
            case VAProfileH264High:
                guid = &guid_h264_vld_nofgt;
                break;
            case VAProfileHEVCMain:
                guid = &guid_hevc_vld_main;
                break;
            case VAProfileHEVCMain10:
                guid = &guid_hevc_vld_main10;
                bitdepth = 10;
                break;
            default:
                continue;
            }

            attrib.type = VAConfigAttribRTFormat;
            if (vaGetConfigAttributes( display, profiles[i], VAEntrypointVLD, &attrib, 1 ) != VA_STATUS_SUCCESS)
                continue;
            if (attrib.value == VA_ATTRIB_NOT_SUPPORTED)
                continue;

            TRACE( "Found usable VA-API profile %d, bitdepth %u.\n", profiles[i], bitdepth );

            params->profiles[count].guid = *guid;
            params->profiles[count].bitdepth = bitdepth;
            count++;
        }
    }

    free( profiles );
    vaTerminate( display );
    close( fd );

    params->count = count;
    return STATUS_SUCCESS;
}

struct decoder_context
{
    VADisplay display;
    int fd;
    VAConfigID config;
    VAContextID context;
    VASurfaceID *surfaces;
    UINT surface_count;
    UINT bitdepth;
    UINT width;
    UINT height;
};

static VAProfile guid_to_profile( const GUID *guid, UINT *bitdepth )
{
    if (IsEqualGUID( guid, &guid_h264_vld_nofgt )) { *bitdepth = 8; return VAProfileH264Main; }
    if (IsEqualGUID( guid, &guid_hevc_vld_main )) { *bitdepth = 8; return VAProfileHEVCMain; }
    if (IsEqualGUID( guid, &guid_hevc_vld_main10 )) { *bitdepth = 10; return VAProfileHEVCMain10; }
    return VAProfileNone;
}

static NTSTATUS decoder_create( void *args )
{
    struct decoder_create_params *params = args;
    struct decoder_context *ctx;
    VAProfile profile;
    UINT bitdepth;
    VADisplay display;
    int fd = -1;
    VAConfigAttrib attrib;
    VAConfigID config;
    VAContextID context;
    VASurfaceID *surfaces;
    unsigned int rt_format;

    params->context = 0;

    if ((profile = guid_to_profile( &params->guid, &bitdepth )) == VAProfileNone)
    {
        WARN( "Unsupported decoder GUID %s.\n", debugstr_guid( &params->guid ) );
        return STATUS_NOT_SUPPORTED;
    }

    if (!(display = va_open( &fd )))
        return STATUS_UNSUCCESSFUL;

    rt_format = bitdepth > 8 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;

    attrib.type = VAConfigAttribRTFormat;
    attrib.value = rt_format;
    if (vaCreateConfig( display, profile, VAEntrypointVLD, &attrib, 1, &config ) != VA_STATUS_SUCCESS)
    {
        WARN( "vaCreateConfig failed.\n" );
        vaTerminate( display );
        close( fd );
        return STATUS_UNSUCCESSFUL;
    }

    if (!(surfaces = calloc( params->surface_count, sizeof(*surfaces) )))
    {
        vaDestroyConfig( display, config );
        vaTerminate( display );
        close( fd );
        return STATUS_NO_MEMORY;
    }

    if (vaCreateSurfaces( display, rt_format, params->width, params->height, surfaces,
            params->surface_count, NULL, 0 ) != VA_STATUS_SUCCESS)
    {
        WARN( "vaCreateSurfaces failed.\n" );
        free( surfaces );
        vaDestroyConfig( display, config );
        vaTerminate( display );
        close( fd );
        return STATUS_UNSUCCESSFUL;
    }

    if (vaCreateContext( display, config, params->width, params->height, VA_PROGRESSIVE,
            surfaces, params->surface_count, &context ) != VA_STATUS_SUCCESS)
    {
        WARN( "vaCreateContext failed.\n" );
        vaDestroySurfaces( display, surfaces, params->surface_count );
        free( surfaces );
        vaDestroyConfig( display, config );
        vaTerminate( display );
        close( fd );
        return STATUS_UNSUCCESSFUL;
    }

    if (!(ctx = malloc( sizeof(*ctx) )))
    {
        vaDestroyContext( display, context );
        vaDestroySurfaces( display, surfaces, params->surface_count );
        free( surfaces );
        vaDestroyConfig( display, config );
        vaTerminate( display );
        close( fd );
        return STATUS_NO_MEMORY;
    }

    ctx->display = display;
    ctx->fd = fd;
    ctx->config = config;
    ctx->context = context;
    ctx->surfaces = surfaces;
    ctx->surface_count = params->surface_count;
    ctx->bitdepth = bitdepth;
    ctx->width = params->width;
    ctx->height = params->height;

    TRACE( "Created decoder context %p, profile %d, %ux%u, %u surfaces.\n",
            ctx, profile, params->width, params->height, params->surface_count );

    params->context = (UINT_PTR)ctx;
    return STATUS_SUCCESS;
}

static NTSTATUS decoder_destroy( void *args )
{
    struct decoder_destroy_params *params = args;
    struct decoder_context *ctx = (struct decoder_context *)(UINT_PTR)params->context;

    if (!ctx) return STATUS_SUCCESS;

    vaDestroyContext( ctx->display, ctx->context );
    vaDestroySurfaces( ctx->display, ctx->surfaces, ctx->surface_count );
    free( ctx->surfaces );
    vaDestroyConfig( ctx->display, ctx->config );
    vaTerminate( ctx->display );
    close( ctx->fd );
    free( ctx );

    return STATUS_SUCCESS;
}

static void copy_plane( BYTE *dst, UINT dst_stride, const BYTE *src, UINT src_stride, UINT height )
{
    UINT row, copy_len = dst_stride < src_stride ? dst_stride : src_stride;

    for (row = 0; row < height; row++)
        memcpy( dst + row * dst_stride, src + row * src_stride, copy_len );
}

static NTSTATUS decoder_decode_frame( void *args )
{
    struct decoder_decode_params *params = args;
    struct decoder_context *ctx = (struct decoder_context *)(UINT_PTR)params->context;
    const DXVA_PicParams_HEVC *pic;
    const DXVA_Slice_HEVC_Short *slices;
    VAPictureParameterBufferHEVC va_pic;
    VAIQMatrixBufferHEVC va_iq;
    VABufferID pic_buf, iq_buf = VA_INVALID_ID;
    VABufferID render_bufs[2];
    VAImage image;
    void *image_data;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    UINT i;

    if (!ctx || params->target_surface_index >= ctx->surface_count)
        return STATUS_INVALID_PARAMETER;
    if (params->pic_params_size < sizeof(*pic))
        return STATUS_INVALID_PARAMETER;

    pic = params->pic_params;
    slices = params->slice_control;

    dxva2_hevc_translate_pic_params( pic, &va_pic, ctx->bitdepth, ctx->bitdepth, ctx->surfaces, ctx->surface_count );

    if (vaCreateBuffer( ctx->display, ctx->context, VAPictureParameterBufferType, sizeof(va_pic), 1, &va_pic,
            &pic_buf ) != VA_STATUS_SUCCESS)
    {
        WARN( "Failed to create picture parameter buffer.\n" );
        return STATUS_UNSUCCESSFUL;
    }

    if (params->qmatrix && params->qmatrix_size >= sizeof(DXVA_Qmatrix_HEVC))
    {
        dxva2_hevc_translate_iq_matrix( params->qmatrix, &va_iq );
        if (vaCreateBuffer( ctx->display, ctx->context, VAIQMatrixBufferType, sizeof(va_iq), 1, &va_iq,
                &iq_buf ) != VA_STATUS_SUCCESS)
        {
            WARN( "Failed to create IQ matrix buffer.\n" );
            iq_buf = VA_INVALID_ID;
        }
    }

    if (vaBeginPicture( ctx->display, ctx->context, ctx->surfaces[params->target_surface_index] ) != VA_STATUS_SUCCESS)
    {
        WARN( "vaBeginPicture failed.\n" );
        return STATUS_UNSUCCESSFUL;
    }

    render_bufs[0] = pic_buf;
    if (iq_buf != VA_INVALID_ID)
    {
        render_bufs[1] = iq_buf;
        vaRenderPicture( ctx->display, ctx->context, render_bufs, 2 );
    }
    else
    {
        vaRenderPicture( ctx->display, ctx->context, render_bufs, 1 );
    }

    for (i = 0; i < params->slice_count; i++)
    {
        const BYTE *nal_data = (const BYTE *)params->bitstream + slices[i].BSNALunitDataLocation;
        UINT nal_size = slices[i].SliceBytesInBuffer;
        VASliceParameterBufferHEVC va_slice;
        VABufferID slice_bufs[2];

        if (slices[i].BSNALunitDataLocation + (UINT64)nal_size > params->bitstream_size)
        {
            WARN( "Slice %u out of bounds of the bitstream buffer.\n", i );
            continue;
        }

        /* DXVA raw-bitstream submissions are Annex B: each slice NAL is
         * prefixed with a start code (FFmpeg-based clients write 00 00 01),
         * but both our header parser and VA-API's slice data buffer expect
         * the NAL to start at the NAL unit header. */
        if (nal_size >= 4 && !nal_data[0] && !nal_data[1] && !nal_data[2] && nal_data[3] == 1)
        {
            nal_data += 4;
            nal_size -= 4;
        }
        else if (nal_size >= 3 && !nal_data[0] && !nal_data[1] && nal_data[2] == 1)
        {
            nal_data += 3;
            nal_size -= 3;
        }

        if (!dxva2_hevc_translate_slice( pic, nal_data, nal_size, &va_slice, 0 ))
        {
            WARN( "Failed to parse slice %u header.\n", i );
            continue;
        }
        if (i == params->slice_count - 1) va_slice.LongSliceFlags.fields.LastSliceOfPic = 1;

        if (vaCreateBuffer( ctx->display, ctx->context, VASliceParameterBufferType, sizeof(va_slice), 1, &va_slice,
                &slice_bufs[0] ) != VA_STATUS_SUCCESS)
        {
            WARN( "Failed to create slice parameter buffer for slice %u.\n", i );
            continue;
        }
        if (vaCreateBuffer( ctx->display, ctx->context, VASliceDataBufferType, nal_size, 1, (void *)nal_data,
                &slice_bufs[1] ) != VA_STATUS_SUCCESS)
        {
            WARN( "Failed to create slice data buffer for slice %u.\n", i );
            continue;
        }

        vaRenderPicture( ctx->display, ctx->context, slice_bufs, 2 );
    }

    vaEndPicture( ctx->display, ctx->context );
    vaSyncSurface( ctx->display, ctx->surfaces[params->target_surface_index] );

    if (vaDeriveImage( ctx->display, ctx->surfaces[params->target_surface_index], &image ) != VA_STATUS_SUCCESS)
    {
        VAImageFormat format;

        /* Some drivers (e.g. nvidia-vaapi-driver) don't support deriving a
         * CPU-mappable image directly from a decode surface - fall back to
         * an explicit vaCreateImage + vaGetImage copy instead. Format must
         * match the RT format the surfaces were created with in
         * decoder_create(). */
        WARN( "vaDeriveImage failed, falling back to vaGetImage.\n" );

        memset( &format, 0, sizeof(format) );
        format.fourcc = ctx->bitdepth > 8 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
        format.byte_order = VA_LSB_FIRST;
        format.bits_per_pixel = ctx->bitdepth > 8 ? 24 : 12;

        if (vaCreateImage( ctx->display, &format, ctx->width, ctx->height, &image ) != VA_STATUS_SUCCESS)
        {
            WARN( "vaCreateImage fallback failed.\n" );
            return STATUS_UNSUCCESSFUL;
        }

        if (vaGetImage( ctx->display, ctx->surfaces[params->target_surface_index], 0, 0, ctx->width, ctx->height,
                image.image_id ) != VA_STATUS_SUCCESS)
        {
            WARN( "vaGetImage fallback failed.\n" );
            vaDestroyImage( ctx->display, image.image_id );
            return STATUS_UNSUCCESSFUL;
        }
    }

    TRACE( "va image: %ux%u, format %#x, num_planes %u, pitches[0]=%u pitches[1]=%u offsets[0]=%u offsets[1]=%u; "
            "output: stride %u height %u (surface requested at decoder_create: %ux%u); pic_params: %ux%u luma samples, "
            "slice_count %u.\n",
            image.width, image.height, image.format.fourcc, image.num_planes, image.pitches[0], image.pitches[1],
            image.offsets[0], image.offsets[1], params->output_stride, params->output_height, ctx->width, ctx->height,
            va_pic.pic_width_in_luma_samples, va_pic.pic_height_in_luma_samples, params->slice_count );

    if (vaMapBuffer( ctx->display, image.buf, &image_data ) == VA_STATUS_SUCCESS)
    {
        UINT bytes_per_sample = ctx->bitdepth > 8 ? 2 : 1;
        UINT copy_height = min( params->output_height, image.height );
        BYTE *luma_dst = params->output;
        BYTE *chroma_dst = (BYTE *)params->output + params->output_stride * params->output_height;

        /* NV12/P010: plane 0 luma, plane 1 interleaved UV at half height. */
        copy_plane( luma_dst, params->output_stride, (BYTE *)image_data + image.offsets[0], image.pitches[0],
                copy_height );
        if (image.num_planes > 1)
            copy_plane( chroma_dst, params->output_stride, (BYTE *)image_data + image.offsets[1], image.pitches[1],
                    copy_height / 2 );
        (void)bytes_per_sample;

        /* The app's D3D9 surface can be taller than the actual decoded picture
         * (e.g. padded to its own alignment boundary independent of the coded
         * size) - fill anything beyond what we actually decoded with black
         * instead of leaving it as stale D3DLOCK_DISCARD memory. */
        if (params->output_height > copy_height)
        {
            UINT extra_luma = params->output_height - copy_height;
            UINT extra_chroma = params->output_height / 2 - copy_height / 2;

            memset( luma_dst + copy_height * params->output_stride, 0, extra_luma * params->output_stride );
            memset( chroma_dst + (copy_height / 2) * params->output_stride, 0x80,
                    extra_chroma * params->output_stride );
        }

        vaUnmapBuffer( ctx->display, image.buf );
        status = STATUS_SUCCESS;
    }
    else
    {
        WARN( "vaMapBuffer failed.\n" );
    }

    vaDestroyImage( ctx->display, image.image_id );

    return status;
}

#else /* HAVE_LIBVA */

static NTSTATUS query_decoder_profiles( void *args )
{
    struct query_decoder_profiles_params *params = args;
    params->count = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS decoder_create( void *args )
{
    struct decoder_create_params *params = args;
    params->context = 0;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS decoder_destroy( void *args )
{
    return STATUS_SUCCESS;
}

static NTSTATUS decoder_decode_frame( void *args )
{
    return STATUS_NOT_SUPPORTED;
}

#endif /* HAVE_LIBVA */

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    query_decoder_profiles,
    decoder_create,
    decoder_destroy,
    decoder_decode_frame,
};

C_ASSERT( ARRAY_SIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

typedef ULONG PTR32;

static NTSTATUS wow64_query_decoder_profiles( void *args )
{
    struct
    {
        PTR32 profiles;
        UINT  capacity;
        UINT  count;
    } *params32 = args;
    struct query_decoder_profiles_params params =
    {
        ULongToPtr(params32->profiles),
        params32->capacity,
        0,
    };
    NTSTATUS status = query_decoder_profiles( &params );
    params32->count = params.count;
    return status;
}

static NTSTATUS wow64_decoder_create( void *args )
{
    struct
    {
        GUID   guid;
        UINT   width;
        UINT   height;
        UINT   surface_count;
        UINT64 context;
    } *params32 = args;
    struct decoder_create_params params =
    {
        params32->guid,
        params32->width,
        params32->height,
        params32->surface_count,
        0,
    };
    NTSTATUS status = decoder_create( &params );
    params32->context = params.context;
    return status;
}

static NTSTATUS wow64_decoder_destroy( void *args )
{
    struct
    {
        UINT64 context;
    } *params32 = args;
    struct decoder_destroy_params params = { params32->context };
    return decoder_destroy( &params );
}

static NTSTATUS wow64_decoder_decode_frame( void *args )
{
    struct
    {
        UINT64 context;
        UINT   target_surface_index;
        PTR32  pic_params;
        UINT   pic_params_size;
        PTR32  qmatrix;
        UINT   qmatrix_size;
        PTR32  slice_control;
        UINT   slice_count;
        PTR32  bitstream;
        UINT   bitstream_size;
        PTR32  output;
        UINT   output_stride;
        UINT   output_height;
    } *params32 = args;
    struct decoder_decode_params params =
    {
        params32->context,
        params32->target_surface_index,
        ULongToPtr(params32->pic_params),
        params32->pic_params_size,
        ULongToPtr(params32->qmatrix),
        params32->qmatrix_size,
        ULongToPtr(params32->slice_control),
        params32->slice_count,
        ULongToPtr(params32->bitstream),
        params32->bitstream_size,
        ULongToPtr(params32->output),
        params32->output_stride,
        params32->output_height,
    };
    return decoder_decode_frame( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_query_decoder_profiles,
    wow64_decoder_create,
    wow64_decoder_destroy,
    wow64_decoder_decode_frame,
};

C_ASSERT( ARRAY_SIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif /* _WIN64 */
