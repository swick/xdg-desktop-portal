/*
 * Copyright © 2025 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

#pragma once

XdpDbusImplRequest * xdp_fiber_impl_request_proxy_new (GDBusConnection  *connection,
                                                       GDBusProxyFlags   flags,
                                                       const gchar      *name,
                                                       const gchar      *object_path,
                                                       GError          **error);

gboolean xdp_fiber_impl_request_close (XdpDbusImplRequest  *proxy,
                                       GError             **error);

struct _XdpFutureWallpaperSkeletonClass
{
  XdpDbusWallpaperSkeletonClass parent_class;
};

#define XDP_TYPE_FUTURE_WALLPAPER_SKELETON xdp_future_wallpaper_skeleton_get_type()
G_DECLARE_DERIVABLE_TYPE (XdpFutureWallpaperSkeleton,
                          xdp_future_wallpaper_skeleton,
                          XDP, FUTURE_WALLPAPER_SKELETON,
                          XdpDbusWallpaperSkeleton)

#define XDP_TYPE_FUTURE_WALLPAPER xdp_future_wallpaper_get_type()
G_DECLARE_INTERFACE (XdpFutureWallpaper,
                     xdp_future_wallpaper,
                     XDP, FUTURE_WALLPAPER,
                     XdpDbusWallpaper)

typedef struct _XdpFutureWallpaperInterface
{
  GTypeInterface parent_iface;

  void (*handle_set_wallpaper_uri) (XdpDbusWallpaper      *object,
                                    GDBusMethodInvocation *invocation,
                                    char                  *arg_parent_window,
                                    char                  *arg_uri,
                                    GVariant              *arg_options);

  void (*handle_set_wallpaper_file) (XdpDbusWallpaper      *object,
                                     GDBusMethodInvocation *invocation,
                                     GUnixFDList           *fd_list,
                                     char                  *arg_parent_window,
                                     GVariant              *arg_fd,
                                     GVariant              *arg_options);
} XdpFutureWallpaperInterface;

void xdp_future_wallpaper_skeleton_cancel (XdpFutureWallpaperSkeleton *skeleton);

gboolean xdp_fiber_impl_wallpaper_set_uri (XdpDbusImplWallpaper  *proxy,
                                           const gchar           *arg_handle,
                                           const gchar           *arg_app_id,
                                           const gchar           *arg_parent_window,
                                           const gchar           *arg_uri,
                                           GVariant              *arg_options,
                                           guint                 *out_response,
                                           GError               **error);

gboolean xdp_fiber_impl_access_dialog (XdpDbusImplAccess  *proxy,
                                       const gchar        *arg_handle,
                                       const gchar        *arg_app_id,
                                       const gchar        *arg_parent_window,
                                       const gchar        *arg_title,
                                       const gchar        *arg_subtitle,
                                       const gchar        *arg_body,
                                       GVariant           *arg_options,
                                       guint              *out_response,
                                       GVariant          **out_results,
                                       GError            **error);
