/*
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

#include <stdint.h>
#include "windef.h"
#include "ntstatus.h"
#include "ntgdi.h"
#include "wine/gdi_driver.h"
#include "wine/metal_driver.h"
#include "wine/unixlib.h"
#include "unixlib.h"

static const struct metal_funcs *metal_funcs;

static NTSTATUS winemetal_init( void *args )
{
    metal_funcs = __wine_get_metal_driver( WINE_METAL_DRIVER_VERSION );
    if (!metal_funcs) return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

static NTSTATUS winemetal_get_hwnd_metal_layer( void *args )
{
    struct winemetal_get_hwnd_metal_layer_params *params = args;
    struct client_surface *client;

    if (!metal_funcs) return STATUS_UNSUCCESSFUL;
    if (!params->hwnd) return STATUS_INVALID_PARAMETER;

    if (!metal_funcs->p_get_hwnd_metal_layer( (HWND)(UINT_PTR)params->hwnd, &params->layer, &client ))
        return STATUS_UNSUCCESSFUL;

    params->surface = (UINT64)(UINT_PTR)client;
    return STATUS_SUCCESS;
}

static NTSTATUS winemetal_release_surface( void *args )
{
    struct winemetal_release_surface_params *params = args;

    if (!params->surface) return STATUS_INVALID_PARAMETER;

    client_surface_release( (struct client_surface *)(UINT_PTR)params->surface );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    winemetal_init,
    winemetal_get_hwnd_metal_layer,
    winemetal_release_surface,
};

C_ASSERT( sizeof(struct winemetal_get_hwnd_metal_layer_params) == 3 * sizeof(UINT64) );
C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_winemetal_funcs_count );

#ifdef _WIN64

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    winemetal_init,
    winemetal_get_hwnd_metal_layer,
    winemetal_release_surface,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_winemetal_funcs_count );

#endif /* _WIN64 */