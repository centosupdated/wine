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

#include <basetsd.h>

struct winemetal_get_hwnd_metal_layer_params
{
    UINT64 hwnd;
    UINT64 layer;
    UINT64 surface;
};

struct winemetal_release_surface_params
{
    UINT64 surface;
};

enum winemetal_funcs
{
    unix_winemetal_init,
    unix_winemetal_get_hwnd_metal_layer,
    unix_winemetal_release_surface,
    unix_winemetal_funcs_count
};