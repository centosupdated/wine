/*
 * MACDRV touch driver
 *
 * Copyright 2026 Shengdun Wang
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

#include "macdrv.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(cursor);


/***********************************************************************
 *              send_touch_input
 *
 * Deliver a touch event to wineserver.  Touch positions are encoded as a
 * fraction of the raw physical screen, matching what the server decodes
 * when it synthesizes mouse input from the touch.
 */
static void send_touch_input(HWND hwnd, unsigned int message, int id, int x, int y)
{
    INPUT input = {0};
    RECT virtual;
    POINT pos;

    virtual = NtUserGetVirtualScreenRect(MDT_RAW_DPI);
    pos.x = x * 65535 / (virtual.right - virtual.left);
    pos.y = y * 65535 / (virtual.bottom - virtual.top);

    input.type = INPUT_HARDWARE;
    input.hi.uMsg = message;
    input.hi.wParamL = id;
    input.hi.wParamH = POINTER_MESSAGE_FLAG_INRANGE | POINTER_MESSAGE_FLAG_INCONTACT;
    if (message == WM_POINTERDOWN) input.hi.wParamH |= POINTER_MESSAGE_FLAG_NEW;

    TRACE("hwnd=%p id=%d message=%x screen=%d,%d pos=%d,%d\n", hwnd, id, message, x, y, pos.x, pos.y);

    NtUserSendHardwareInput(hwnd, 0, &input, MAKELPARAM(pos.x, pos.y));
}


/***********************************************************************
 *              macdrv_touch
 *
 * Handler for TOUCH events.
 */
void macdrv_touch(HWND hwnd, const macdrv_event *event)
{
    unsigned int message;

    TRACE("win %p/%p id %d phase %d at (%d,%d) time %lu (%lu ticks ago)\n", hwnd, event->window,
          event->touch.id, event->touch.phase, event->touch.x, event->touch.y,
          event->touch.time_ms, (NtGetTickCount() - event->touch.time_ms));

    switch (event->touch.phase)
    {
    case 0: message = WM_POINTERDOWN; break;
    case 1: message = WM_POINTERUPDATE; break;
    case 2: message = WM_POINTERUP; break;
    default:
        WARN("unrecognized touch phase %d\n", event->touch.phase);
        return;
    }

    send_touch_input(hwnd, message, event->touch.id, event->touch.x, event->touch.y);
}
