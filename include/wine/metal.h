/*
 * Wine Metal API
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

#ifndef __WINE_METAL_H
#define __WINE_METAL_H

#include <windef.h>
#include <basetsd.h>

typedef UINT64 WINE_METAL_LAYER;
typedef UINT64 WINE_METAL_SURFACE;

WINE_METAL_LAYER WINAPI WineMetalGetHwndMetalLayer( HWND hwnd, WINE_METAL_SURFACE *surface );
void WINAPI WineMetalReleaseSurface( WINE_METAL_SURFACE surface );

#endif /* __WINE_METAL_H */