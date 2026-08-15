/* Mac Driver Metal implementation
 *
 * Copyright 2026 Marc-Aurel Zent for CodeWeavers
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

#include "ntstatus.h"
#include "macdrv.h"
#include "wine/debug.h"
#include "wine/metal_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(metal);

static BOOL macdrv_get_hwnd_metal_layer( HWND hwnd, UINT64 *layer, struct client_surface **client )
{
    struct macdrv_client_surface *surface;

    TRACE( "hwnd %p\n", hwnd );

    if (!hwnd) return FALSE;

    if (!(surface = macdrv_client_surface_create( hwnd ))) return FALSE;

    if (!macdrv_client_surface_acquire_metal_swapchain( surface ))
    {
        client_surface_release( &surface->client );
        return FALSE;
    }

    *layer = (UINT64)(UINT_PTR)macdrv_swapchain_get_layer( surface->metal_swapchain );
    *client = &surface->client;
    return TRUE;
}

static const struct metal_driver_funcs macdrv_metal_driver_funcs =
{
    .p_get_hwnd_metal_layer = macdrv_get_hwnd_metal_layer,
};

UINT macdrv_MetalInit( UINT version, const struct metal_funcs *metal_funcs, const struct metal_driver_funcs **driver_funcs )
{
    macdrv_metal_device device;

    (void)metal_funcs;

    if (version != WINE_METAL_DRIVER_VERSION)
    {
        ERR( "version mismatch, win32u wants %u but driver has %u\n", version, WINE_METAL_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }

    if (!(device = macdrv_create_metal_device()))
    {
        TRACE( "Metal not available\n" );
        return STATUS_NOT_SUPPORTED;
    }
    macdrv_release_metal_device( device );

    *driver_funcs = &macdrv_metal_driver_funcs;
    return STATUS_SUCCESS;
}