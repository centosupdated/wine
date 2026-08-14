/*
 * Wayland touch handling
 *
 * Copyright (c) 2026 cqwrteur
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

#include <stdlib.h>
#include <string.h>

#include "waylanddrv.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(input);

static struct wayland_touch_slot *wayland_touch_find_slot(int32_t id)
{
    struct wayland_touch *touch = &process_wayland.touch;
    unsigned int i;

    for (i = 0; i < WAYLAND_TOUCH_MAX_SLOTS; i++)
        if (touch->slots[i].used && touch->slots[i].id == id) return &touch->slots[i];
    return NULL;
}

static struct wayland_touch_slot *wayland_touch_find_free_slot(void)
{
    struct wayland_touch *touch = &process_wayland.touch;
    unsigned int i;

    for (i = 0; i < WAYLAND_TOUCH_MAX_SLOTS; i++)
        if (!touch->slots[i].used) return &touch->slots[i];
    return NULL;
}

static void send_touch_input(struct wayland_touch_slot *slot, unsigned int message)
{
    INPUT input = {0};
    RECT virtual;
    POINT screen, pos;
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    UINT flags = 0;

    if (!(data = wayland_win_data_get(slot->hwnd))) return;
    if (!(surface = data->wayland_surface))
    {
        wayland_win_data_release(data);
        return;
    }

    screen.x = wl_fixed_to_double(slot->x);
    screen.y = wl_fixed_to_double(slot->y);
    screen = map_point_from_surface(surface, screen);
    screen.x += surface->window.rect.left;
    screen.y += surface->window.rect.top;

    wayland_win_data_release(data);

    virtual = NtUserGetVirtualScreenRect(MDT_RAW_DPI);
    pos.x = screen.x * 65535 / (virtual.right - virtual.left);
    pos.y = screen.y * 65535 / (virtual.bottom - virtual.top);

    input.type = INPUT_HARDWARE;
    input.hi.uMsg = message;
    if (message == WM_POINTERDOWN) flags |= POINTER_MESSAGE_FLAG_NEW;
    input.hi.wParamL = slot->id;
    input.hi.wParamH = POINTER_MESSAGE_FLAG_INRANGE | POINTER_MESSAGE_FLAG_INCONTACT | flags;

    TRACE("hwnd=%p id=%d message=%x screen_xy=%d,%d\n", slot->hwnd, slot->id, message, screen.x, screen.y);

    NtUserSendHardwareInput(slot->hwnd, 0, &input, MAKELPARAM(pos.x, pos.y));
}

static void touch_handle_down(void *data, struct wl_touch *wl_touch, uint32_t serial,
                              uint32_t time, struct wl_surface *wl_surface, int32_t id,
                              wl_fixed_t x, wl_fixed_t y)
{
    struct wayland_touch_slot *slot;

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!wl_surface) return;

    pthread_mutex_lock(&process_wayland.touch.mutex);
    slot = wayland_touch_find_slot(id);
    if (!slot) slot = wayland_touch_find_free_slot();
    if (slot)
    {
        slot->id = id;
        slot->hwnd = wl_surface_get_user_data(wl_surface);
        slot->x = x;
        slot->y = y;
        slot->used = TRUE;
        process_wayland.touch.serial = serial;
        process_wayland.touch.serial_id = id;
        process_wayland.touch.serial_hwnd = slot->hwnd;
    }
    pthread_mutex_unlock(&process_wayland.touch.mutex);

    if (!slot) return;

    send_touch_input(slot, WM_POINTERDOWN);
}

static void touch_handle_up(void *data, struct wl_touch *wl_touch, uint32_t serial,
                            uint32_t time, int32_t id)
{
    struct wayland_touch_slot *slot;

    InterlockedExchange(&process_wayland.input_serial, serial);

    pthread_mutex_lock(&process_wayland.touch.mutex);
    slot = wayland_touch_find_slot(id);
    if (slot)
    {
        slot->used = FALSE;
        if (process_wayland.touch.serial_id == id)
        {
            process_wayland.touch.serial = 0;
            process_wayland.touch.serial_id = -1;
            process_wayland.touch.serial_hwnd = NULL;
        }
    }
    pthread_mutex_unlock(&process_wayland.touch.mutex);

    if (!slot) return;

    send_touch_input(slot, WM_POINTERUP);
}

static void touch_handle_motion(void *data, struct wl_touch *wl_touch, uint32_t time,
                                int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    struct wayland_touch_slot *slot;

    pthread_mutex_lock(&process_wayland.touch.mutex);
    slot = wayland_touch_find_slot(id);
    if (slot)
    {
        slot->x = x;
        slot->y = y;
    }
    pthread_mutex_unlock(&process_wayland.touch.mutex);

    if (!slot) return;

    send_touch_input(slot, WM_POINTERUPDATE);
}

static void touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
    struct wayland_touch_slot *slot;
    unsigned int i;

    for (i = 0; i < WAYLAND_TOUCH_MAX_SLOTS; i++)
    {
        pthread_mutex_lock(&process_wayland.touch.mutex);
        slot = &process_wayland.touch.slots[i];
        if (!slot->used)
        {
            pthread_mutex_unlock(&process_wayland.touch.mutex);
            continue;
        }
        slot->used = FALSE;
        if (process_wayland.touch.serial_id == slot->id)
        {
            process_wayland.touch.serial = 0;
            process_wayland.touch.serial_id = -1;
            process_wayland.touch.serial_hwnd = NULL;
        }
        pthread_mutex_unlock(&process_wayland.touch.mutex);
        send_touch_input(slot, WM_POINTERUP);
    }
}

static void touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
}

static const struct wl_touch_listener touch_listener =
{
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel
};

void wayland_touch_init(struct wl_touch *wl_touch)
{
    struct wayland_touch *touch = &process_wayland.touch;

    pthread_mutex_lock(&touch->mutex);
    touch->wl_touch = wl_touch;
    pthread_mutex_unlock(&touch->mutex);
    wl_touch_add_listener(touch->wl_touch, &touch_listener, NULL);
}

void wayland_touch_deinit(void)
{
    struct wayland_touch *touch = &process_wayland.touch;

    pthread_mutex_lock(&touch->mutex);
    if (touch->wl_touch)
    {
        wl_touch_release(touch->wl_touch);
        touch->wl_touch = NULL;
    }
    memset(touch->slots, 0, sizeof(touch->slots));
    pthread_mutex_unlock(&touch->mutex);
}
