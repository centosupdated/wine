/*
 * Wayland client-side window decorations
 *
 * Draws an Adwaita-style title bar into the window surface for windows
 * that the compositor does not decorate with server-side decorations
 * (e.g. GNOME/Mutter, which doesn't implement the xdg-decoration protocol).
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

#include <string.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "waylanddrv.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

/* Adwaita light theme colors */
#define CSD_LIGHT_BG_ACTIVE_TOP     0xfff6f5f4
#define CSD_LIGHT_BG_ACTIVE_BOTTOM  0xffdeddda
#define CSD_LIGHT_BG_INACTIVE_TOP   0xfff6f5f4
#define CSD_LIGHT_BG_INACTIVE_BOTTOM 0xffe8e7e6
#define CSD_LIGHT_FG               0xff2e3436
#define CSD_LIGHT_FG_INACTIVE      0xff92928f
#define CSD_LIGHT_BORDER           0xffc0bfbc
#define CSD_LIGHT_BUTTON_BG        0xffdcddda
#define CSD_LIGHT_BUTTON_FG        0xff2e3436

/* Adwaita dark theme colors */
#define CSD_DARK_BG_ACTIVE_TOP     0xff383838
#define CSD_DARK_BG_ACTIVE_BOTTOM  0xff2b2b2b
#define CSD_DARK_BG_INACTIVE_TOP   0xff2f2f2f
#define CSD_DARK_BG_INACTIVE_BOTTOM 0xff262626
#define CSD_DARK_FG               0xffeeeeec
#define CSD_DARK_FG_INACTIVE      0xff9c9c99
#define CSD_DARK_BORDER           0xff1c1c1c
#define CSD_DARK_BUTTON_BG        0xff414141
#define CSD_DARK_BUTTON_FG        0xffeeeeec

static FT_Library csd_ft_library;
static FT_Face csd_ft_face;
static char *csd_font_file;
static pthread_mutex_t csd_font_mutex = PTHREAD_MUTEX_INITIALIZER;

static BOOL csd_font_init(void)
{
    FcPattern *pattern, *match;
    FcChar8 *file = NULL;
    FcConfig *config;
    FcResult result;
    BOOL ret = TRUE;

    pthread_mutex_lock(&csd_font_mutex);
    if (csd_ft_library) goto out;

    if (!FcInit()) { ret = FALSE; goto out; }
    if (!(config = FcInitLoadConfigAndFonts())) { ret = FALSE; goto out; }

    if (!(pattern = FcNameParse((const FcChar8 *)"sans-serif")))
    {
        ret = FALSE;
        goto out;
    }
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    if (!(match = FcFontMatch(config, pattern, &result)))
    {
        FcPatternDestroy(pattern);
        ret = FALSE;
        goto out;
    }
    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file)
    {
        FcPatternDestroy(match);
        FcPatternDestroy(pattern);
        ret = FALSE;
        goto out;
    }
    csd_font_file = strdup((const char *)file);
    FcPatternDestroy(match);
    FcPatternDestroy(pattern);

    if (FT_Init_FreeType(&csd_ft_library) ||
        FT_New_Face(csd_ft_library, csd_font_file, 0, &csd_ft_face) ||
        FT_Select_Charmap(csd_ft_face, FT_ENCODING_UNICODE))
    {
        if (csd_ft_face)
        {
            FT_Done_Face(csd_ft_face);
            csd_ft_face = NULL;
        }
        if (csd_ft_library)
        {
            FT_Done_FreeType(csd_ft_library);
            csd_ft_library = NULL;
        }
        ret = FALSE;
    }

out:
    pthread_mutex_unlock(&csd_font_mutex);
    return ret;
}

static void csd_put_pixel(UINT32 *pixels, int stride, int x, int y, UINT32 color)
{
    if (x < 0 || y < 0 || x >= stride) return;
    pixels[y * stride + x] = color;
}

static void csd_draw_hline(UINT32 *pixels, int stride, int x0, int x1, int y, UINT32 color)
{
    int x;
    for (x = max(x0, 0); x < min(x1, stride); x++) csd_put_pixel(pixels, stride, x, y, color);
}

static void csd_draw_vline(UINT32 *pixels, int stride, int y0, int y1, int x, UINT32 color)
{
    int y;
    for (y = y0; y < y1; y++) csd_put_pixel(pixels, stride, x, y, color);
}

static void csd_draw_circle(UINT32 *pixels, int stride, int cx, int cy, int radius, UINT32 color)
{
    int x, y;
    for (y = -radius; y <= radius; y++)
    {
        for (x = -radius; x <= radius; x++)
        {
            if (x * x + y * y <= radius * radius)
                csd_put_pixel(pixels, stride, cx + x, cy + y, color);
        }
    }
}

static void csd_blend_pixel(UINT32 *pixels, int stride, int x, int y, UINT32 fg, BYTE alpha)
{
    UINT32 bg;
    UINT32 r, g, b;

    if (x < 0 || y < 0 || x >= stride || !alpha) return;

    bg = pixels[y * stride + x];
    r = ((fg >> 16) & 0xff) * alpha + ((bg >> 16) & 0xff) * (255 - alpha);
    g = ((fg >> 8) & 0xff) * alpha + ((bg >> 8) & 0xff) * (255 - alpha);
    b = (fg & 0xff) * alpha + (bg & 0xff) * (255 - alpha);
    pixels[y * stride + x] = 0xff000000 | ((r / 255) << 16) | ((g / 255) << 8) | (b / 255);
}

static int csd_text_width(const WCHAR *text)
{
    int width = 0;
    while (*text)
    {
        FT_UInt glyph = FT_Get_Char_Index(csd_ft_face, *text++);
        if (glyph)
        {
            FT_Load_Glyph(csd_ft_face, glyph, FT_LOAD_DEFAULT);
            width += csd_ft_face->glyph->advance.x >> 6;
        }
    }
    return width;
}

static void csd_draw_text(UINT32 *pixels, int stride, const WCHAR *text, int x, int y, UINT32 color)
{
    int pen = x;
    while (*text)
    {
        FT_UInt glyph = FT_Get_Char_Index(csd_ft_face, *text++);
        if (!glyph) continue;
        if (FT_Load_Glyph(csd_ft_face, glyph, FT_LOAD_DEFAULT)) continue;
        if (FT_Render_Glyph(csd_ft_face->glyph, FT_RENDER_MODE_NORMAL)) continue;

        {
            FT_Bitmap *bitmap = &csd_ft_face->glyph->bitmap;
            int glyph_x = pen + csd_ft_face->glyph->bitmap_left;
            /* bitmap_top is the distance from the baseline up to the top of
             * the bitmap, so the bitmap's top edge in buffer coordinates is
             * baseline - bitmap_top. */
            int glyph_y = y - csd_ft_face->glyph->bitmap_top;
            int i, j;

            for (j = 0; j < bitmap->rows; j++)
            {
                for (i = 0; i < bitmap->width; i++)
                {
                    BYTE alpha = bitmap->buffer[j * bitmap->width + i];
                    csd_blend_pixel(pixels, stride, glyph_x + i, glyph_y + j, color, alpha);
                }
            }
        }
        pen += csd_ft_face->glyph->advance.x >> 6;
    }
}

enum csd_button_type
{
    CSD_BUTTON_MINIMIZE,
    CSD_BUTTON_MAXIMIZE,
    CSD_BUTTON_CLOSE
};

static void csd_clear_rounded_corner(UINT32 *pixels, int stride, int x0, int y0, int radius, int cx, int cy)
{
    int x, y;

    for (y = 0; y < radius; y++)
    {
        for (x = 0; x < radius; x++)
        {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > radius * radius)
                pixels[(y0 + y) * stride + (x0 + x)] = 0; /* transparent */
        }
    }
}

static void csd_draw_button(UINT32 *pixels, int stride, const RECT *rect,
                            enum csd_button_type type, UINT32 button_bg,
                            UINT32 button_fg)
{
    int cx = (rect->left + rect->right) / 2;
    int cy = (rect->top + rect->bottom) / 2;
    int size = min(rect->right - rect->left, rect->bottom - rect->top);
    int radius = size * 2 / 5;
    int s = radius * 2 / 3;

    csd_draw_circle(pixels, stride, cx, cy, radius, button_bg);

    switch (type)
    {
    case CSD_BUTTON_MINIMIZE:
        csd_draw_hline(pixels, stride, cx - s, cx + s + 1, cy, button_fg);
        csd_draw_hline(pixels, stride, cx - s, cx + s + 1, cy + 1, button_fg);
        break;
    case CSD_BUTTON_MAXIMIZE:
        csd_draw_hline(pixels, stride, cx - s, cx + s + 1, cy - s, button_fg);
        csd_draw_hline(pixels, stride, cx - s, cx + s + 1, cy + s, button_fg);
        csd_draw_vline(pixels, stride, cy - s, cy + s + 1, cx - s, button_fg);
        csd_draw_vline(pixels, stride, cy - s, cy + s + 1, cx + s, button_fg);
        break;
    case CSD_BUTTON_CLOSE:
    {
        int i;
        for (i = 0; i <= s; i++)
        {
            csd_put_pixel(pixels, stride, cx - s + i, cy - s + i, button_fg);
            csd_put_pixel(pixels, stride, cx + s - i, cy - s + i, button_fg);
            csd_put_pixel(pixels, stride, cx - s + i, cy + s - i, button_fg);
            csd_put_pixel(pixels, stride, cx + s - i, cy + s - i, button_fg);
        }
        break;
    }
    }
}

/**********************************************************************
 *          wayland_csd_should_draw
 *
 * Return TRUE if a client-side title bar should be drawn for the window.
 */
BOOL wayland_csd_should_draw(struct wayland_win_data *data)
{
    struct wayland_surface *surface = data->wayland_surface;

    if (!process_wayland.managed_mode || !process_wayland.decorated_mode) return FALSE;
    if (!surface || !wayland_surface_is_toplevel(surface)) return FALSE;
    if (surface->decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) return FALSE;
    /* only draw a title bar if the window actually has a caption area */
    if (data->rects.window.top == data->rects.client.top) return FALSE;
    return TRUE;
}

BOOL wayland_csd_should_draw_win(HWND hwnd)
{
    struct wayland_win_data *data;
    BOOL ret = FALSE;

    if ((data = wayland_win_data_get(hwnd)))
    {
        ret = wayland_csd_should_draw(data);
        wayland_win_data_release(data);
    }
    return ret;
}

void wayland_csd_set_active(HWND hwnd, BOOL active)
{
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (data->csd_active != active)
    {
        data->csd_active = active;
        wayland_win_data_release(data);
        NtUserExposeWindowSurface(hwnd, 0, NULL);
        return;
    }
    wayland_win_data_release(data);
}

/**********************************************************************
 *          wayland_csd_paint
 *
 * Paint an Adwaita-style title bar over the caption strip of the window
 * surface. The caption strip occupies the rows [0, client_top) of the
 * surface buffer, where client_top is the offset of the client area
 * within the surface.
 *
 * This must not call back into win32u, since it runs from the window
 * surface flush which is itself invoked from win32u. All data it needs
 * is cached in the wayland_win_data.
 */
void wayland_csd_paint(struct wayland_shm_buffer *buffer, struct wayland_win_data *data, int client_top)
{
    UINT32 *pixels = buffer->map_data;
    int stride = buffer->width;
    RECT btn_rect;
    UINT32 top_color, bottom_color, fg_color, fg_inactive, border, button_bg, button_fg;
    WCHAR *title = data->csd_title;
    int title_len, i;
    INT caption_cy, btn_cx;
    BOOL active = data->csd_active;
    DWORD style = data->csd_style;
    DWORD ex_style = data->csd_ex_style;
    int bar_height, content_w;

    if (process_wayland.dark_theme)
    {
        top_color = active ? CSD_DARK_BG_ACTIVE_TOP : CSD_DARK_BG_INACTIVE_TOP;
        bottom_color = active ? CSD_DARK_BG_ACTIVE_BOTTOM : CSD_DARK_BG_INACTIVE_BOTTOM;
        fg_color = CSD_DARK_FG;
        fg_inactive = CSD_DARK_FG_INACTIVE;
        border = CSD_DARK_BORDER;
        button_bg = CSD_DARK_BUTTON_BG;
        button_fg = CSD_DARK_BUTTON_FG;
    }
    else
    {
        top_color = active ? CSD_LIGHT_BG_ACTIVE_TOP : CSD_LIGHT_BG_INACTIVE_TOP;
        bottom_color = active ? CSD_LIGHT_BG_ACTIVE_BOTTOM : CSD_LIGHT_BG_INACTIVE_BOTTOM;
        fg_color = CSD_LIGHT_FG;
        fg_inactive = CSD_LIGHT_FG_INACTIVE;
        border = CSD_LIGHT_BORDER;
        button_bg = CSD_LIGHT_BUTTON_BG;
        button_fg = CSD_LIGHT_BUTTON_FG;
    }

    bar_height = min(client_top, buffer->height);
    if (bar_height <= 0) return;

    /* The buffer covers the visible rect rounded up to 128 pixels, but only
     * the actual visible rect (the window content) is displayed. Position
     * the title and buttons within that content area. */
    content_w = data->rects.visible.right - data->rects.visible.left;

    TRACE("hwnd=%p painting titlebar height=%d active=%d buffer=%dx%d content=%dx%d dark=%d\n",
          data->hwnd, bar_height, active, buffer->width, buffer->height, content_w, bar_height, process_wayland.dark_theme);

    /* vertical gradient */
    for (i = 0; i < bar_height; i++)
    {
        int r, g, b, denom = bar_height - 1;
        int tr = (int)((top_color >> 16) & 0xff), br = (int)((bottom_color >> 16) & 0xff);
        int tg = (int)((top_color >> 8) & 0xff), bg = (int)((bottom_color >> 8) & 0xff);
        int tb = (int)(top_color & 0xff), bb = (int)(bottom_color & 0xff);
        if (denom <= 0) denom = 1;
        r = tr + (br - tr) * i / denom;
        g = tg + (bg - tg) * i / denom;
        b = tb + (bb - tb) * i / denom;
        csd_draw_hline(pixels, stride, 0, buffer->width, i,
                       0xff000000 | (r << 16) | (g << 8) | b);
    }

    /* bottom border */
    if (bar_height < buffer->height)
        csd_draw_hline(pixels, stride, 0, buffer->width, bar_height, border);

    /* window title, centered */
    title_len = lstrlenW(title);
    if (title_len > 0 && csd_font_init())
    {
        int width, x, baseline, font_size;

        pthread_mutex_lock(&csd_font_mutex);

        /* Scale the title with the caption height so it is prominent
         * regardless of the window's DPI. */
        font_size = bar_height * 3 / 5;
        if (font_size < 8) font_size = 8;
        if (font_size > bar_height - 6) font_size = bar_height - 6;
        FT_Set_Pixel_Sizes(csd_ft_face, 0, font_size);
        width = csd_text_width(title);
        x = (content_w - width) / 2;
        if (x < 0) x = 0;
        baseline = (bar_height - ((csd_ft_face->size->metrics.ascender - csd_ft_face->size->metrics.descender) >> 6)) / 2 +
                   (csd_ft_face->size->metrics.ascender >> 6);
        TRACE("title=%s font_size=%d width=%d x=%d baseline=%d ascender=%d bar_height=%d\n",
              wine_dbgstr_w(title), font_size, width, x, baseline, (int)(csd_ft_face->size->metrics.ascender >> 6), bar_height);
        csd_draw_text(pixels, stride, title, x, baseline, active ? fg_color : fg_inactive);

        pthread_mutex_unlock(&csd_font_mutex);
    }

    /* buttons on the right, matching win32u caption hit-test positions */
    caption_cy = bar_height;
    btn_cx = bar_height;
    if (caption_cy < 1) caption_cy = bar_height;

    btn_rect.top = 0;
    btn_rect.bottom = min(caption_cy, bar_height);
    /* Inset from the right edge so the buttons stay clear of the rounded
     * top-right corner. */
    btn_rect.right = content_w - bar_height / 3;

    if (style & WS_SYSMENU)
    {
        btn_rect.left = btn_rect.right - caption_cy;
        csd_draw_button(pixels, stride, &btn_rect, CSD_BUTTON_CLOSE, button_bg, button_fg);
        btn_rect.right = btn_rect.left;
    }
    if ((style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) && !(ex_style & WS_EX_TOOLWINDOW))
    {
        btn_rect.left = btn_rect.right - btn_cx;
        csd_draw_button(pixels, stride, &btn_rect, CSD_BUTTON_MAXIMIZE, button_bg, button_fg);
        btn_rect.right = btn_rect.left;
        btn_rect.left = btn_rect.right - btn_cx;
        csd_draw_button(pixels, stride, &btn_rect, CSD_BUTTON_MINIMIZE, button_bg, button_fg);
    }

    /* Round the top corners of the window so it matches the desktop theme.
     * The circle that rounds a corner is centered just inside the window:
     * at the bottom-right of the top-left corner region and at the
     * bottom-left of the top-right corner region.  Skip the rounding when
     * maximized so the window meets the screen edges squarely. */
    if (bar_height >= 9 && !(style & WS_MAXIMIZE))
    {
        int corner = bar_height / 3;
        csd_clear_rounded_corner(pixels, stride, 0, 0, corner, corner, corner);
        csd_clear_rounded_corner(pixels, stride, content_w - corner, 0, corner, 0, corner);
    }
}
