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

enum unix_funcs
{
    unix_query_decoder_profiles,
    unix_funcs_count,
};

#define DXVA2_CALL( func, params ) WINE_UNIX_CALL( unix_ ## func, params )

#endif /* __DXVA2_UNIXLIB_H */
