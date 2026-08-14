/*
 * Wayland core handling
 *
 * Copyright (c) 2020 Alexandros Frantzis for Collabora Ltd
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

#include "waylanddrv.h"

#include "ntstatus.h"
#include "winreg.h"

#include "wine/debug.h"

#include <stdlib.h>

#ifdef SONAME_LIBDBUS_1
#include <dlfcn.h>
#include <dbus/dbus.h>
#endif

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct wayland process_wayland =
{
    .seat.mutex = PTHREAD_MUTEX_INITIALIZER,
    .keyboard.mutex = PTHREAD_MUTEX_INITIALIZER,
    .pointer.mutex = PTHREAD_MUTEX_INITIALIZER,
    .text_input.mutex = PTHREAD_MUTEX_INITIALIZER,
    .data_device.mutex = PTHREAD_MUTEX_INITIALIZER,
    .output_list = {&process_wayland.output_list, &process_wayland.output_list},
    .output_mutex = PTHREAD_MUTEX_INITIALIZER
};

static inline void ascii_to_unicode( WCHAR *dst, const char *src, size_t len )
{
    while (len--) *dst++ = (unsigned char)*src++;
}

#define IS_OPTION_TRUE(ch) ((ch) == 'y' || (ch) == 'Y' || (ch) == 't' || (ch) == 'T' || (ch) == '1')

static inline UINT asciiz_to_unicode( WCHAR *dst, const char *src )
{
    WCHAR *p = dst;
    while ((*p++ = *src++));
    return (p - dst) * sizeof(WCHAR);
}

static HKEY reg_open_key( HKEY root, const WCHAR *name, ULONG name_len )
{
    UNICODE_STRING nameW = { name_len, name_len, (WCHAR *)name };
    OBJECT_ATTRIBUTES attr;
    HANDLE ret;

    attr.Length = sizeof(attr);
    attr.RootDirectory = root;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    return NtOpenKeyEx( &ret, MAXIMUM_ALLOWED, &attr, 0 ) ? 0 : ret;
}

static HKEY open_hkcu_key( const char *name )
{
    WCHAR bufferW[256];
    static HKEY hkcu;

    if (!hkcu)
    {
        char buffer[256];
        DWORD_PTR sid_data[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD_PTR)];
        DWORD i, len = sizeof(sid_data);
        SID *sid;

        if (NtQueryInformationToken( GetCurrentThreadEffectiveToken(), TokenUser, sid_data, len, &len ))
            return 0;

        sid = ((TOKEN_USER *)sid_data)->User.Sid;
        len = sprintf( buffer, "\\Registry\\User\\S-%u-%u", sid->Revision,
                       MAKELONG( MAKEWORD( sid->IdentifierAuthority.Value[5],
                                           sid->IdentifierAuthority.Value[4] ),
                                 MAKEWORD( sid->IdentifierAuthority.Value[3],
                                           sid->IdentifierAuthority.Value[2] )));
        for (i = 0; i < sid->SubAuthorityCount; i++)
            len += sprintf( buffer + len, "-%u", sid->SubAuthority[i] );

        ascii_to_unicode( bufferW, buffer, len );
        hkcu = reg_open_key( NULL, bufferW, len * sizeof(WCHAR) );
    }

    return reg_open_key( hkcu, bufferW, asciiz_to_unicode( bufferW, name ) - sizeof(WCHAR) );
}

static ULONG query_reg_value( HKEY hkey, const WCHAR *name, KEY_VALUE_PARTIAL_INFORMATION *info, ULONG size )
{
    unsigned int name_size = name ? lstrlenW( name ) * sizeof(WCHAR) : 0;
    UNICODE_STRING nameW = { name_size, name_size, (WCHAR *)name };

    if (NtQueryValueKey( hkey, &nameW, KeyValuePartialInformation,
                         info, size, &size ))
        return 0;

    return size - FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);
}

static DWORD get_config_key( HKEY defkey, HKEY appkey, const char *name, WCHAR *buffer, DWORD size )
{
    WCHAR nameW[128];
    char buf[2048];
    KEY_VALUE_PARTIAL_INFORMATION *info = (void *)buf;

    asciiz_to_unicode( nameW, name );

    if (appkey && query_reg_value( appkey, nameW, info, sizeof(buf) ))
    {
        size = min( info->DataLength, size - sizeof(WCHAR) );
        memcpy( buffer, info->Data, size );
        buffer[size / sizeof(WCHAR)] = 0;
        return 0;
    }

    if (defkey && query_reg_value( defkey, nameW, info, sizeof(buf) ))
    {
        size = min( info->DataLength, size - sizeof(WCHAR) );
        memcpy( buffer, info->Data, size );
        buffer[size / sizeof(WCHAR)] = 0;
        return 0;
    }

    return ERROR_FILE_NOT_FOUND;
}

static void wayland_read_wm_settings(void)
{
    static const WCHAR x11driverW[] = {'\\','X','1','1',' ','D','r','i','v','e','r',0};
    WCHAR buffer[MAX_PATH + 16], *p, *appname;
    HKEY hkey, appkey = 0;
    DWORD len;

    /* @@ Wine registry key: HKCU\Software\Wine\X11 Driver */
    hkey = open_hkcu_key( "Software\\Wine\\X11 Driver" );

    /* open the app-specific key */
    appname = RtlGetCurrentPeb()->ProcessParameters->ImagePathName.Buffer;
    if ((p = wcsrchr( appname, '/' ))) appname = p + 1;
    if ((p = wcsrchr( appname, '\\' ))) appname = p + 1;
    len = lstrlenW( appname );

    if (len && len < MAX_PATH)
    {
        HKEY tmpkey;
        int i;
        for (i = 0; appname[i]; i++) buffer[i] = RtlDowncaseUnicodeChar( appname[i] );
        buffer[i] = 0;
        appname = buffer;
        memcpy( appname + i, x11driverW, sizeof(x11driverW) );
        /* @@ Wine registry key: HKCU\Software\Wine\AppDefaults\app.exe\X11 Driver */
        if ((tmpkey = open_hkcu_key( "Software\\Wine\\AppDefaults" )))
        {
            appkey = reg_open_key( tmpkey, appname, lstrlenW( appname ) * sizeof(WCHAR) );
            NtClose( tmpkey );
        }
    }

    process_wayland.managed_mode = TRUE;
    if (!get_config_key( hkey, appkey, "Managed", buffer, sizeof(buffer) ))
        process_wayland.managed_mode = IS_OPTION_TRUE( buffer[0] );

    process_wayland.decorated_mode = TRUE;
    if (!get_config_key( hkey, appkey, "Decorated", buffer, sizeof(buffer) ))
        process_wayland.decorated_mode = IS_OPTION_TRUE( buffer[0] );

    if (appkey) NtClose( appkey );
    if (hkey) NtClose( hkey );

    TRACE( "managed=%d decorated=%d\n", process_wayland.managed_mode, process_wayland.decorated_mode );
}

#ifdef SONAME_LIBDBUS_1

#define WAYLAND_DBUS_FUNCS \
    DO_FUNC(dbus_bus_get_private); \
    DO_FUNC(dbus_connection_close); \
    DO_FUNC(dbus_connection_flush); \
    DO_FUNC(dbus_connection_send_with_reply_and_block); \
    DO_FUNC(dbus_connection_unref); \
    DO_FUNC(dbus_error_free); \
    DO_FUNC(dbus_error_init); \
    DO_FUNC(dbus_message_iter_append_basic); \
    DO_FUNC(dbus_message_iter_get_arg_type); \
    DO_FUNC(dbus_message_iter_get_basic); \
    DO_FUNC(dbus_message_iter_init); \
    DO_FUNC(dbus_message_iter_init_append); \
    DO_FUNC(dbus_message_iter_next); \
    DO_FUNC(dbus_message_iter_recurse); \
    DO_FUNC(dbus_message_new_method_call); \
    DO_FUNC(dbus_message_unref); \
    DO_FUNC(dbus_set_error_const)

#define DO_FUNC(f) static typeof(f) *p_##f
WAYLAND_DBUS_FUNCS;
#undef DO_FUNC

static BOOL wayland_load_dbus_functions(void)
{
    void *handle;

    if (!(handle = dlopen(SONAME_LIBDBUS_1, RTLD_NOW))) return FALSE;

#define DO_FUNC(f) if (!(p_##f = dlsym(handle, #f))) return FALSE
    WAYLAND_DBUS_FUNCS;
#undef DO_FUNC
    return TRUE;
}

static BOOL wayland_query_dark_theme(void)
{
    static const char *bus_name = "org.freedesktop.portal.Desktop";
    static const char *path = "/org/freedesktop/portal/desktop";
    static const char *iface = "org.freedesktop.portal.Settings";
    static const char *namespace = "org.freedesktop.appearance";
    static const char *key = "color-scheme";
    DBusConnection *connection;
    DBusMessage *request, *reply;
    DBusMessageIter iter, variant;
    DBusError error;
    dbus_uint32_t value = 0;
    BOOL dark = FALSE;

    if (!wayland_load_dbus_functions()) return FALSE;

    p_dbus_error_init(&error);
    if (!(connection = p_dbus_bus_get_private(DBUS_BUS_SESSION, &error)))
    {
        TRACE("failed to connect to session bus: %s\n", error.message);
        p_dbus_error_free(&error);
        return FALSE;
    }
    p_dbus_error_free(&error);

    if ((request = p_dbus_message_new_method_call(bus_name, path, iface, "Read")))
    {
        p_dbus_message_iter_init_append(request, &iter);
        p_dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &namespace);
        p_dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &key);

        p_dbus_error_init(&error);
        reply = p_dbus_connection_send_with_reply_and_block(connection, request, 1000, &error);
        p_dbus_message_unref(request);
        if (reply)
        {
            p_dbus_error_free(&error);
            p_dbus_message_iter_init(reply, &iter);
            /* Unwrap any layers of variant wrapping. */
            while (p_dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT)
            {
                p_dbus_message_iter_recurse(&iter, &variant);
                iter = variant;
            }
            if (p_dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32)
                p_dbus_message_iter_get_basic(&iter, &value);
            p_dbus_message_unref(reply);
        }
        else
        {
            TRACE("failed to read color scheme: %s\n", error.message);
            p_dbus_error_free(&error);
        }
    }

    p_dbus_connection_flush(connection);
    p_dbus_connection_close(connection);
    p_dbus_connection_unref(connection);

    dark = (value == 1); /* 0 = NoPreference, 1 = PreferDark, 2 = PreferLight */
    TRACE("color scheme value=%u dark=%d\n", value, dark);
    return dark;
}

#endif /* SONAME_LIBDBUS_1 */

/**********************************************************************
 *          xdg_wm_base handling
 */

static void xdg_wm_base_handle_ping(void *data, struct xdg_wm_base *shell,
                                    uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener =
{
    xdg_wm_base_handle_ping
};

/**********************************************************************
 *          wl_seat handling
 */

static void wl_seat_handle_capabilities(void *data, struct wl_seat *seat,
                                        enum wl_seat_capability caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !process_wayland.pointer.wl_pointer)
        wayland_pointer_init(wl_seat_get_pointer(seat));
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && process_wayland.pointer.wl_pointer)
        wayland_pointer_deinit();

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !process_wayland.touch.wl_touch)
        wayland_touch_init(wl_seat_get_touch(seat));
    else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && process_wayland.touch.wl_touch)
        wayland_touch_deinit();

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !process_wayland.keyboard.wl_keyboard)
        wayland_keyboard_init(wl_seat_get_keyboard(seat));
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && process_wayland.keyboard.wl_keyboard)
        wayland_keyboard_deinit();
}

static void wl_seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener =
{
    wl_seat_handle_capabilities,
    wl_seat_handle_name
};

/**********************************************************************
 *          Registry handling
 */

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t id, const char *interface,
                                   uint32_t version)
{
    TRACE("interface=%s version=%u id=%u\n", interface, version, id);

    if (strcmp(interface, "wl_output") == 0)
    {
        if (!wayland_output_create(id, version))
            ERR("Failed to create wayland_output for global id=%u\n", id);
    }
    else if (strcmp(interface, "zxdg_output_manager_v1") == 0)
    {
        struct wayland_output *output;

        process_wayland.zxdg_output_manager_v1 =
            wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface,
                             version < 3 ? version : 3);

        /* Add zxdg_output_v1 to existing outputs. */
        wl_list_for_each(output, &process_wayland.output_list, link)
            wayland_output_use_xdg_extension(output);
    }
    else if (strcmp(interface, "zxdg_decoration_manager_v1") == 0)
    {
        process_wayland.zxdg_decoration_manager_v1 =
            wl_registry_bind(registry, id, &zxdg_decoration_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "wl_compositor") == 0)
    {
        process_wayland.wl_compositor =
            wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, "xdg_wm_base") == 0)
    {
        /* Bind version 2 so that compositors (e.g., sway) can properly send tiled
         * states, instead of falling back to (ab)using the maximized state. */
        process_wayland.xdg_wm_base =
            wl_registry_bind(registry, id, &xdg_wm_base_interface,
                             version < 2 ? version : 2);
        xdg_wm_base_add_listener(process_wayland.xdg_wm_base, &xdg_wm_base_listener, NULL);
    }
    else if (strcmp(interface, "wl_shm") == 0)
    {
        process_wayland.wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, "wl_seat") == 0)
    {
        struct wayland_seat *seat = &process_wayland.seat;
        if (seat->wl_seat)
        {
            WARN("Only a single seat is currently supported, ignoring additional seats.\n");
            return;
        }
        pthread_mutex_lock(&seat->mutex);
        seat->wl_seat = wl_registry_bind(registry, id, &wl_seat_interface,
                                         version < 8 ? version : 8);
        seat->global_id = id;
        wl_seat_add_listener(seat->wl_seat, &seat_listener, NULL);
        pthread_mutex_unlock(&seat->mutex);
        if (process_wayland.zwp_text_input_manager_v3) wayland_text_input_init();
        /* Recreate the data device for the new seat. */
        if (process_wayland.data_device.zwlr_data_control_device_v1 ||
            process_wayland.data_device.wl_data_device)
        {
            wayland_data_device_init();
        }
    }
    else if (strcmp(interface, "wp_viewporter") == 0)
    {
        process_wayland.wp_viewporter =
            wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
    }
    else if (strcmp(interface, "wl_subcompositor") == 0)
    {
        process_wayland.wl_subcompositor =
            wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    }
    else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0)
    {
        process_wayland.zwp_pointer_constraints_v1 =
            wl_registry_bind(registry, id, &zwp_pointer_constraints_v1_interface, 1);
    }
    else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0)
    {
        process_wayland.zwp_relative_pointer_manager_v1 =
            wl_registry_bind(registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "zwp_text_input_manager_v3") == 0)
    {
        process_wayland.zwp_text_input_manager_v3 =
            wl_registry_bind(registry, id, &zwp_text_input_manager_v3_interface, 1);
        if (process_wayland.seat.wl_seat) wayland_text_input_init();
    }
    else if (strcmp(interface, "zwlr_data_control_manager_v1") == 0)
    {
        process_wayland.zwlr_data_control_manager_v1 =
            wl_registry_bind(registry, id, &zwlr_data_control_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "wl_data_device_manager") == 0)
    {
        process_wayland.wl_data_device_manager =
            wl_registry_bind(registry, id, &wl_data_device_manager_interface, 2);
    }
    else if (strcmp(interface, "xdg_toplevel_icon_manager_v1") == 0)
    {
        process_wayland.xdg_toplevel_icon_manager_v1 =
            wl_registry_bind(registry, id, &xdg_toplevel_icon_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_cursor_shape_manager_v1") == 0)
    {
        process_wayland.wp_cursor_shape_manager_v1 =
            wl_registry_bind(registry, id, &wp_cursor_shape_manager_v1_interface,
                             version < 2 ? version : 2);
    }
    else if (strcmp(interface, "wp_pointer_warp_v1") == 0)
    {
        process_wayland.wp_pointer_warp_v1 =
            wl_registry_bind(registry, id, &wp_pointer_warp_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_alpha_modifier_v1") == 0)
    {
        process_wayland.wp_alpha_modifier_v1 =
            wl_registry_bind(registry, id, &wp_alpha_modifier_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_fractional_scale_manager_v1") == 0)
    {
        process_wayland.wp_fractional_scale_manager_v1 =
            wl_registry_bind(registry, id, &wp_fractional_scale_manager_v1_interface, 1);
    }
#ifdef WL_FIXES_ACK_GLOBAL_REMOVE
    else if (strcmp(interface, "wl_fixes") == 0)
    {
        if (version < 2)
            return;

        process_wayland.wl_fixes =
            wl_registry_bind(registry, id, &wl_fixes_interface, 2);
    }
#endif
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                          uint32_t id)
{
    struct wayland_output *output, *tmp;
    struct wayland_seat *seat;

    TRACE("id=%u\n", id);

#ifdef WL_FIXES_ACK_GLOBAL_REMOVE
    if (process_wayland.wl_fixes)
        wl_fixes_ack_global_remove(process_wayland.wl_fixes, registry, id);
#endif

    wl_list_for_each_safe(output, tmp, &process_wayland.output_list, link)
    {
        if (output->global_id == id)
        {
            TRACE("removing output->name=%s\n", output->current.name);
            wayland_output_destroy(output);
            return;
        }
    }

    seat = &process_wayland.seat;
    if (seat->wl_seat && seat->global_id == id)
    {
        TRACE("removing seat\n");
        if (process_wayland.pointer.wl_pointer) wayland_pointer_deinit();
        if (process_wayland.touch.wl_touch) wayland_touch_deinit();
        if (process_wayland.text_input.zwp_text_input_v3) wayland_text_input_deinit();
        pthread_mutex_lock(&seat->mutex);
        wl_seat_release(seat->wl_seat);
        seat->wl_seat = NULL;
        seat->global_id = 0;
        pthread_mutex_unlock(&seat->mutex);
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

/**********************************************************************
 *          wayland_process_init
 *
 *  Initialise the per process wayland objects.
 *
 */
BOOL wayland_process_init(void)
{
    struct wl_display *wl_display_wrapper;

    process_wayland.wl_display = wl_display_connect(NULL);
    if (!process_wayland.wl_display)
        return FALSE;

    TRACE("wl_display=%p\n", process_wayland.wl_display);

#if (WAYLAND_VERSION_MAJOR == 1 && WAYLAND_VERSION_MINOR >= 23)
    if (!(process_wayland.wl_event_queue =
          wl_display_create_queue_with_name(process_wayland.wl_display, process_name ? process_name : "winewayland")))
#else
    if (!(process_wayland.wl_event_queue = wl_display_create_queue(process_wayland.wl_display)))
#endif
    {
        ERR("Failed to create event queue\n");
        return FALSE;
    }

    if (!(wl_display_wrapper = wl_proxy_create_wrapper(process_wayland.wl_display)))
    {
        ERR("Failed to create proxy wrapper for wl_display\n");
        return FALSE;
    }
    wl_proxy_set_queue((struct wl_proxy *) wl_display_wrapper,
                       process_wayland.wl_event_queue);

    process_wayland.wl_registry = wl_display_get_registry(wl_display_wrapper);
    wl_proxy_wrapper_destroy(wl_display_wrapper);
    if (!process_wayland.wl_registry)
    {
        ERR("Failed to get to wayland registry\n");
        return FALSE;
    }

    wayland_window_init();

    /* Populate registry */
    wl_registry_add_listener(process_wayland.wl_registry, &registry_listener, NULL);

    /* We need two roundtrips. One to get and bind globals, one to handle all
     * initial events produced from registering the globals. */
    wl_display_roundtrip_queue(process_wayland.wl_display, process_wayland.wl_event_queue);
    wl_display_roundtrip_queue(process_wayland.wl_display, process_wayland.wl_event_queue);

    /* Check for required protocol globals. */
    if (!process_wayland.wl_compositor)
    {
        ERR("Wayland compositor doesn't support wl_compositor\n");
        return FALSE;
    }
    if (!process_wayland.xdg_wm_base)
    {
        ERR("Wayland compositor doesn't support xdg_wm_base\n");
        return FALSE;
    }
    if (!process_wayland.wl_shm)
    {
        ERR("Wayland compositor doesn't support wl_shm\n");
        return FALSE;
    }
    if (!process_wayland.wl_subcompositor)
    {
        ERR("Wayland compositor doesn't support wl_subcompositor\n");
        return FALSE;
    }
    if (!process_wayland.wp_viewporter)
    {
        ERR("Wayland compositor doesn't support wp_viewporter\n");
        return FALSE;
    }

    /* Check for optional globals. */
    if (!process_wayland.zwp_pointer_constraints_v1)
        ERR("Wayland compositor doesn't support optional zwp_pointer_constraints_v1 (pointer locking/confining won't work)\n");

    if (!process_wayland.zwp_relative_pointer_manager_v1)
        ERR("Wayland compositor doesn't support optional zwp_relative_pointer_manager_v1 (relative motion won't work)\n");

    if (!process_wayland.zwp_text_input_manager_v3)
        ERR("Wayland compositor doesn't support optional zwp_text_input_manager_v3 (host input methods won't work)\n");

    if (!process_wayland.zwlr_data_control_manager_v1)
    {
        if (!process_wayland.wl_data_device_manager)
            ERR("Wayland compositor doesn't support optional wl_data_device_manager (clipboard won't work)\n");
        else
            ERR("Wayland compositor doesn't support optional zwlr_data_control_manager_v1 (clipboard functionality will be limited)\n");
    }

    if (!process_wayland.xdg_toplevel_icon_manager_v1)
        ERR("Wayland compositor doesn't support xdg_toplevel_icon_manager_v1 (window icons will not be supported)\n");

    if (!process_wayland.wp_fractional_scale_manager_v1)
        ERR("Wayland compositor doesn't support wp_fractional_scale_manager_v1 (fractional scaling will be broken)\n");

#ifdef SONAME_LIBDBUS_1
    process_wayland.dark_theme = wayland_query_dark_theme();
#else
    process_wayland.dark_theme = FALSE;
#endif

    process_wayland.sm_caption_height = NtUserGetSystemMetrics(SM_CYCAPTION);
    process_wayland.sm_cx_size = NtUserGetSystemMetrics(SM_CXSIZE);
    process_wayland.sm_menu_height = NtUserGetSystemMetrics(SM_CYMENU);

    wayland_read_wm_settings();

    process_wayland.initialized = TRUE;

    return TRUE;
}
