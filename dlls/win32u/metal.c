/*
 * Metal display driver loading
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

#include <pthread.h>

#include "ntstatus.h"
#include "win32u_private.h"
#include "ntuser_private.h"

#include "wine/metal_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(metal);

static const struct metal_driver_funcs *driver_funcs;
static struct metal_funcs metal_funcs;

static BOOL nulldrv_get_hwnd_metal_layer( HWND hwnd, UINT64 *layer, struct client_surface **client )
{
    return FALSE;
}

static const struct metal_driver_funcs nulldrv_funcs =
{
    .p_get_hwnd_metal_layer = nulldrv_get_hwnd_metal_layer,
};

static void metal_driver_init(void)
{
    UINT status;

    if ((status = user_driver->pMetalInit( WINE_METAL_DRIVER_VERSION, &metal_funcs, &driver_funcs )) &&
        status != STATUS_NOT_IMPLEMENTED)
    {
        ERR( "Failed to initialize the driver metal functions, status %#x\n", status );
        return;
    }

    if (status == STATUS_NOT_IMPLEMENTED) driver_funcs = &nulldrv_funcs;
}

static void metal_driver_load(void)
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;
    pthread_once( &init_once, metal_driver_init );
}

static BOOL lazydrv_get_hwnd_metal_layer( HWND hwnd, UINT64 *layer, struct client_surface **client )
{
    metal_driver_load();
    return driver_funcs->p_get_hwnd_metal_layer( hwnd, layer, client );
}

static const struct metal_driver_funcs lazydrv_funcs =
{
    .p_get_hwnd_metal_layer = lazydrv_get_hwnd_metal_layer,
};

static BOOL win32u_get_hwnd_metal_layer( HWND hwnd, UINT64 *layer, struct client_surface **client )
{
    struct client_surface *ret_client;

    metal_driver_load();
    if (!driver_funcs->p_get_hwnd_metal_layer( hwnd, layer, &ret_client )) return FALSE;

    add_window_client_surface( hwnd, ret_client );
    *client = ret_client;
    return TRUE;
}

static void metal_init_once(void)
{
    driver_funcs = &lazydrv_funcs;
    metal_funcs.p_get_hwnd_metal_layer = win32u_get_hwnd_metal_layer;
}

/***********************************************************************
 *      __wine_get_metal_driver  (win32u.so)
 */
const struct metal_funcs *__wine_get_metal_driver( UINT version )
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;

    if (version != WINE_METAL_DRIVER_VERSION)
    {
        ERR( "version mismatch, metal wants %u but win32u has %u\n", version, WINE_METAL_DRIVER_VERSION );
        return NULL;
    }

    pthread_once( &init_once, metal_init_once );
    return &metal_funcs;
}