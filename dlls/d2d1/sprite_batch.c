/*
 * Copyright 2026 Santino Mazza for CodeWeavers
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

#include "d2d1_private.h"
#include <windows.h>

WINE_DEFAULT_DEBUG_CHANNEL(d2d);

static const D2D1_MATRIX_3X2_F identity =
{{{
    1.0f, 0.0f,
    0.0f, 1.0f,
    0.0f, 0.0f,
}}};

static BOOL array_reserve(void **elements, size_t *capacity, size_t count, size_t size)
{
    unsigned int max_capacity, new_capacity;
    void *new_elements;

    if (count <= *capacity)
        return TRUE;

    max_capacity = ~0u / size;
    if (count > max_capacity)
        return FALSE;

    new_capacity = max(8, *capacity);
    while (new_capacity < count && new_capacity <= max_capacity / 2)
        new_capacity *= 2;
    if (new_capacity < count)
        new_capacity = count;

    if (!(new_elements = realloc(*elements, new_capacity * size)))
    {
        ERR("Failed to allocate memory.\n");
        return FALSE;
    }

    *elements = new_elements;
    *capacity = new_capacity;
    return TRUE;
}

static inline struct d2d_sprite_batch *impl_from_ID2D1SpriteBatch(ID2D1SpriteBatch *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_sprite_batch, ID2D1SpriteBatch_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_sprite_batch_QueryInterface(ID2D1SpriteBatch *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1SpriteBatch)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1SpriteBatch_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_sprite_batch_AddRef(ID2D1SpriteBatch *iface)
{
    struct d2d_sprite_batch *batch = impl_from_ID2D1SpriteBatch(iface);
    ULONG refcount = InterlockedIncrement(&batch->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_sprite_batch_Release(ID2D1SpriteBatch *iface)
{
    struct d2d_sprite_batch *batch = impl_from_ID2D1SpriteBatch(iface);
    ULONG refcount = InterlockedDecrement(&batch->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        ID2D1Factory_Release(batch->factory);
        free(batch->sprites);
        free(batch);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d2d_sprite_batch_GetFactory(ID2D1SpriteBatch *iface, ID2D1Factory **factory)
{
    struct d2d_sprite_batch *batch = impl_from_ID2D1SpriteBatch(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    *factory = batch->factory;
    ID2D1Factory_AddRef(*factory);
}

static HRESULT STDMETHODCALLTYPE d2d_sprite_batch_AddSprites(ID2D1SpriteBatch *iface, UINT32 sprite_count,
        const D2D1_RECT_F *destination_rectangles, const D2D1_RECT_U *source_rectangles, const D2D1_COLOR_F *colors,
        const D2D1_MATRIX_3X2_F *transforms, UINT32 destination_rectangles_stride, UINT32 source_rectangles_stride,
        UINT32 colors_stride, UINT32 transforms_stride)
{
    struct d2d_sprite_batch *batch = impl_from_ID2D1SpriteBatch(iface);
    struct d2d_sprite *sprite;

    TRACE("iface %p, sprite_count %u, destination_rectangles %p, source_rectangles %p,"
          "colors %p, transforms %p, destination_rectangles_stride %u,"
          "source_rectangles_stride %u, colors_stride %u, transforms_stride %u.\n",
          iface, sprite_count, destination_rectangles, source_rectangles, colors, transforms,
          destination_rectangles_stride, source_rectangles_stride, colors_stride, transforms_stride);

    if (!destination_rectangles)
        return S_OK;

    if (colors)
        FIXME("Color mask not implemented\n");

    array_reserve((void**)&batch->sprites, &batch->sprites_allocated, batch->sprite_count + sprite_count, sizeof(struct d2d_sprite));

    for (int i = 0; i < sprite_count; ++i)
    {
        sprite = &batch->sprites[i + batch->sprite_count];

        sprite->destination_rectangle = *(D2D1_RECT_F *)(((UCHAR*)destination_rectangles) + i * destination_rectangles_stride);

        if (source_rectangles)
            sprite->source_rectangle = *(D2D1_RECT_U *)(((UCHAR*)source_rectangles) + i * source_rectangles_stride);
        else
            sprite->source_rectangle = (D2D1_RECT_U){0, 0, UINT_MAX, UINT_MAX};

        if (transforms)
            sprite->transform_matrix = *(D2D1_MATRIX_3X2_F *)(((UCHAR*)transforms) + i * transforms_stride);
        else
            sprite->transform_matrix = identity;
    }

    batch->sprite_count += sprite_count;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_sprite_batch_SetSprites(ID2D1SpriteBatch *iface, UINT32 start_index, UINT32 sprite_count,
        const D2D1_RECT_F *destination_rectangles, const D2D1_RECT_U *source_rectangles, const D2D1_COLOR_F *colors,
        const D2D1_MATRIX_3X2_F *transforms, UINT32 destination_rectangles_stride, UINT32 source_rectangles_stride,
        UINT32 colors_stride, UINT32 transforms_stride)
{
    struct d2d_sprite_batch *batch = impl_from_ID2D1SpriteBatch(iface);
    struct d2d_sprite *sprite;

    TRACE("iface %p, start_index %u, sprite_count %u, destination_rectangles %p,"
         "source_rectangles %p, colors %p, transforms %p, destination_rectangles_stride %u,"
          "source_rectangles_stride %u, colors_stride %u, transforms_stride %u.\n",
          iface, start_index, sprite_count, destination_rectangles, source_rectangles, colors, transforms,
          destination_rectangles_stride, source_rectangles_stride, colors_stride, transforms_stride);

    if (start_index >= batch->sprite_count || sprite_count > batch->sprite_count - start_index)
        return E_INVALIDARG;

    if (!sprite_count || (!destination_rectangles && !source_rectangles && !colors && !transforms))
        return S_OK;

    if (colors)
        FIXME("Color mask not implemented\n");

    for (int i = start_index; i < start_index + sprite_count; ++i)
    {
        sprite = &batch->sprites[i];
        if (destination_rectangles)
            sprite->destination_rectangle = *(D2D1_RECT_F *)(((UCHAR*)destination_rectangles) + i * destination_rectangles_stride);

        if (source_rectangles)
            sprite->source_rectangle = *(D2D1_RECT_U *)(((UCHAR*)source_rectangles) + i * source_rectangles_stride);

        if (transforms)
            sprite->transform_matrix = *(D2D1_MATRIX_3X2_F *)(((UCHAR*)transforms) + i * transforms_stride);
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_sprite_batch_GetSprites(ID2D1SpriteBatch *iface, UINT32 start_index, UINT32 sprite_count,
        D2D1_RECT_F *destination_rectangles, D2D1_RECT_U *source_rectangles, D2D1_COLOR_F *colors,
        D2D1_MATRIX_3X2_F *transforms)
{
    struct d2d_sprite_batch *batch = impl_from_ID2D1SpriteBatch(iface);
    struct d2d_sprite *sprite;

    TRACE("iface %p, start_index %u, sprite_count %u, destination_rectangles %p, source_rectangles %p,"
          "colors %p, transforms %p.\n", iface, start_index, sprite_count, destination_rectangles,
          source_rectangles, colors, transforms);

    if (!sprite_count)
        return S_OK;

    if (start_index >= batch->sprite_count || sprite_count > batch->sprite_count - start_index)
        return E_INVALIDARG;

    for (int i = start_index; i < start_index + sprite_count; ++i)
    {
        sprite = &batch->sprites[i];

        if (destination_rectangles)
            destination_rectangles[i] = sprite->destination_rectangle;

        if (source_rectangles)
            source_rectangles[i] = sprite->source_rectangle;
        if (transforms)
            transforms[i] = sprite->transform_matrix;
    }

    return S_OK;
}

static UINT32 STDMETHODCALLTYPE d2d_sprite_batch_GetSpritesCount(ID2D1SpriteBatch *iface)
{
    struct d2d_sprite_batch *batch = impl_from_ID2D1SpriteBatch(iface);
    TRACE("iface %p.\n", iface);
    return batch->sprite_count;
}

static void STDMETHODCALLTYPE d2d_sprite_batch_Clear(ID2D1SpriteBatch *iface)
{
    struct d2d_sprite_batch *batch = impl_from_ID2D1SpriteBatch(iface);
    TRACE("iface %p.\n", iface);

    batch->sprite_count = 0;
}

static const struct ID2D1SpriteBatchVtbl d2d_sprite_batch_vtbl =
{
    d2d_sprite_batch_QueryInterface,
    d2d_sprite_batch_AddRef,
    d2d_sprite_batch_Release,
    d2d_sprite_batch_GetFactory,
    d2d_sprite_batch_AddSprites,
    d2d_sprite_batch_SetSprites,
    d2d_sprite_batch_GetSprites,
    d2d_sprite_batch_GetSpritesCount,
    d2d_sprite_batch_Clear
};

HRESULT d2d_create_sprite_batch(struct d2d_device_context *context, struct d2d_sprite_batch **out)
{
    struct d2d_sprite_batch *sprite_batch;

    *out = NULL;
    if (!(sprite_batch = calloc(1, sizeof(*sprite_batch))))
        return E_OUTOFMEMORY;

    sprite_batch->ID2D1SpriteBatch_iface.lpVtbl = &d2d_sprite_batch_vtbl;

    sprite_batch->refcount = 1;
    ID2D1Factory_AddRef(sprite_batch->factory = context->factory);

    *out = sprite_batch;

    return S_OK;
}

struct d2d_sprite_batch *unsafe_impl_from_ID2D1SpriteBatch(ID2D1SpriteBatch *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d2d_sprite_batch_vtbl);
    return CONTAINING_RECORD(iface, struct d2d_sprite_batch, ID2D1SpriteBatch_iface);
}
