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

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/metal.h"
#include "wine/unixlib.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(metal);

#define UNIX_CALL( func, params ) WINE_UNIX_CALL( unix_winemetal_##func, params )

static BOOL WINAPI winemetal_init( INIT_ONCE *once, void *param, void **context )
{
    return !__wine_init_unix_call() && !UNIX_CALL( init, NULL );
}

static BOOL winemetal_init_once(void)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

    return InitOnceExecuteOnce( &init_once, winemetal_init, NULL, NULL );
}

WINE_METAL_LAYER WINAPI WineMetalGetHwndMetalLayer( HWND hwnd, WINE_METAL_SURFACE *surface )
{
    struct winemetal_get_hwnd_metal_layer_params params = {0};
    NTSTATUS status;

    TRACE( "hwnd %p, surface %p\n", hwnd, surface );

    if (!surface) return 0;

    if (!winemetal_init_once()) return 0;

    params.hwnd = (UINT64)(UINT_PTR)hwnd;
    params.layer = 0;
    params.surface = 0;

    status = UNIX_CALL( get_hwnd_metal_layer, &params );
    if (status) return 0;

    *surface = params.surface;
    return params.layer;
}

void WINAPI WineMetalReleaseSurface( WINE_METAL_SURFACE surface )
{
    struct winemetal_release_surface_params params;

    TRACE( "surface %#lx\n", surface );

    if (!surface) return;

    if (!winemetal_init_once()) return;

    params.surface = surface;
    UNIX_CALL( release_surface, &params );
}