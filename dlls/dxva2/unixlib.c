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
#include <fcntl.h>
#include <unistd.h>

#ifdef HAVE_LIBVA
#include <va/va.h>
#include <va/va_drm.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "wine/debug.h"
#include "unixlib.h"

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

#else /* HAVE_LIBVA */

static NTSTATUS query_decoder_profiles( void *args )
{
    struct query_decoder_profiles_params *params = args;
    params->count = 0;
    return STATUS_SUCCESS;
}

#endif /* HAVE_LIBVA */

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    query_decoder_profiles,
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

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_query_decoder_profiles,
};

C_ASSERT( ARRAY_SIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif /* _WIN64 */
