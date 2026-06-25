/*
 * Metal display driver interface
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

#ifndef __WINE_METAL_DRIVER_H
#define __WINE_METAL_DRIVER_H

#include <basetsd.h>

#define WINE_METAL_DRIVER_VERSION 3

#ifdef WINE_UNIX_LIB

struct client_surface;

struct metal_driver_funcs
{
    BOOL (*p_get_hwnd_metal_layer)( HWND hwnd, UINT64 *layer, struct client_surface **client );
};

struct metal_funcs
{
    BOOL (*p_get_hwnd_metal_layer)( HWND hwnd, UINT64 *layer, struct client_surface **client );
};

#endif /* WINE_UNIX_LIB */

#endif /* __WINE_METAL_DRIVER_H */