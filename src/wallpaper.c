/*
 * Copyright © 2019 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
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
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Felipe Borges <feborges@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>
#include <libdex.h>

#include "wallpaper.h"
#include "xdp-permissions.h"
#include "xdp-request.h"
#include "xdp-dbus.h"
#include "xdp-dbus-wrappers.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "wallpaper"
#define PERMISSION_ID "wallpaper"

typedef struct _Wallpaper Wallpaper;
typedef struct _WallpaperClass WallpaperClass;

struct _Wallpaper
{
  XdpDbusWallpaperSkeleton parent_instance;
};

struct _WallpaperClass
{
  XdpDbusWallpaperSkeletonClass parent_class;
};

static XdpDbusImplWallpaper *impl;
static XdpDbusImplAccess *access_impl;
static Wallpaper *wallpaper;

GType wallpaper_get_type (void) G_GNUC_CONST;
static void wallpaper_iface_init (XdpDbusWallpaperIface *iface);

G_DEFINE_TYPE_WITH_CODE (Wallpaper, wallpaper, XDP_DBUS_TYPE_WALLPAPER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_WALLPAPER,
                                                wallpaper_iface_init));

typedef struct _SetWallpaperRequest
{
  XdpRequest *request;
  char *uri;
  char *parent_window;
  GVariant *options;
  XdgDesktopPortalResponseEnum response;
} SetWallpaperRequest;

static SetWallpaperRequest *
set_wallpaper_request_new (XdpRequest *request,
                           const char *uri,
                           const char *parent_window,
                           GVariant   *options)
{
  SetWallpaperRequest *wpr = g_new0 (SetWallpaperRequest, 1);

  wpr->request = g_object_ref (request);
  wpr->uri = g_strdup (uri);
  wpr->parent_window = g_strdup (parent_window);
  wpr->options = g_variant_ref (options);
  wpr->response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;

  return wpr;
}

static void
set_wallpaper_request_free (SetWallpaperRequest *wpr)
{
  g_clear_object (&wpr->request);
  g_clear_pointer (&wpr->uri, g_free);
  g_clear_pointer (&wpr->parent_window, g_free);
  g_clear_pointer (&wpr->options, g_variant_unref);
  g_clear_pointer (&wpr->options, g_variant_unref);

  g_free (wpr);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SetWallpaperRequest, set_wallpaper_request_free)

static gboolean
validate_set_on (const char *key,
                 GVariant *value,
                 GVariant *options,
                 GError **error)
{
  const char *string = g_variant_get_string (value, NULL);

  return ((g_strcmp0 (string, "both") == 0) ||
          (g_strcmp0 (string, "background") == 0) ||
          (g_strcmp0 (string, "lockscreen") == 0));
}

static XdpOptionKey wallpaper_options[] = {
  { "show-preview", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "set-on", G_VARIANT_TYPE_STRING, validate_set_on }
};

static DexFuture*
handle_set_wallpaper (gpointer user_data)
{
  SetWallpaperRequest *wallpaper_request = user_data;
  XdpRequest *request = wallpaper_request->request;
  const char *id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  char *parent_window = NULL;
  char *uri = NULL;
  g_auto(GVariantBuilder) opt_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  GVariant *options;
  gboolean show_preview = FALSE;
  XdpPermission permission;

  parent_window = wallpaper_request->parent_window;
  uri = wallpaper_request->uri;
  options = wallpaper_request->options;

  permission = xdp_fiber_get_permission (id, PERMISSION_TABLE, PERMISSION_ID);
  if (permission == XDP_PERMISSION_NO)
    return NULL;

  g_variant_lookup (options, "show-preview", "b", &show_preview);
  if (!show_preview && permission != XDP_PERMISSION_YES)
    {
      guint access_response = 2;
      g_autoptr(GVariant) access_results = NULL;
      g_auto(GVariantBuilder) access_opt_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      g_autofree gchar *app_id = NULL;
      g_autofree gchar *title = NULL;
      g_autofree gchar *subtitle = NULL;
      const gchar *body;

      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "deny_label", g_variant_new_string (_("Deny")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "grant_label", g_variant_new_string (_("Allow")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "icon", g_variant_new_string ("preferences-desktop-wallpaper-symbolic"));

      if (g_strcmp0 (id, "") != 0)
        {
          GAppInfo *info = xdp_app_info_get_gappinfo (request->app_info);
          const gchar *name = NULL;

          if (info)
            {
              name = g_app_info_get_display_name (G_APP_INFO (info));
              app_id = xdp_get_app_id_from_desktop_id (g_app_info_get_id (info));
            }
          else
            {
              name = id;
              app_id = g_strdup (id);
            }

          title = g_strdup_printf (_("Allow %s to Set Backgrounds?"), name);
          subtitle = g_strdup_printf (_("%s is requesting to be able to change the background image."), name);
        }
      else
        {
          /* Note: this will set the wallpaper permission for all unsandboxed
           * apps for which an app ID can't be determined.
           */
          g_assert (xdp_app_info_is_host (request->app_info));
          app_id = g_strdup ("");
          title = g_strdup (_("Allow Applications to Set Backgrounds?"));
          subtitle = g_strdup (_("An application is requesting to be able to change the background image."));
        }
      body = _("This permission can be changed at any time from the privacy settings.");

    if (!xdp_fiber_impl_access_dialog (access_impl,
                                       request->id,
                                       app_id,
                                       parent_window,
                                       title,
                                       subtitle,
                                       body,
                                       g_variant_builder_end (&access_opt_builder),
                                       &access_response,
                                       NULL,
                                       &error))
        {
          g_warning ("Failed to show access dialog: %s", error->message);
          return dex_future_new_for_error (g_steal_pointer (&error));
        }

      if (permission == XDP_PERMISSION_UNSET)
        xdp_set_permission_sync (id, PERMISSION_TABLE, PERMISSION_ID, access_response == 0 ? XDP_PERMISSION_YES : XDP_PERMISSION_NO);

      if (access_response != 0)
        return NULL;
    }

  impl_request = xdp_fiber_impl_request_proxy_new (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                   G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                   g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                   request->id,
                                                   &error);
  if (!impl_request)
    {
      g_warning ("Failed to to create wallpaper implementation proxy: %s", error->message);
      return dex_future_new_for_error (g_steal_pointer (&error));
    }

  xdp_request_set_impl_request (request, impl_request);

  xdp_filter_options (options, &opt_builder,
                      wallpaper_options, G_N_ELEMENTS (wallpaper_options),
                      NULL);

  guint backend_response = 2;
  g_debug ("Calling SetWallpaperURI with %s", uri);

  if (!xdp_fiber_impl_wallpaper_set_uri (impl,
                                         request->id,
                                         id,
                                         parent_window,
                                         uri,
                                         g_variant_builder_end (&opt_builder),
                                         &backend_response,
                                         &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("A backend call failed: %s", error->message);
      return dex_future_new_for_error (g_steal_pointer (&error));
    }

  wallpaper_request->response = backend_response;
  return NULL;
}

static DexFuture *
set_wallpaper_request_done (DexFuture *future,
                            gpointer   user_data)
{
  SetWallpaperRequest *wallpaper_request = user_data;
  XdpRequest *request = wallpaper_request->request;

  if (request->exported)
    {
      g_auto(GVariantBuilder) opt_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      g_debug ("sending response: %d", wallpaper_request->response);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      wallpaper_request->response,
                                      g_variant_builder_end (&opt_builder));
      xdp_request_unexport (request);
    }

  return NULL;
}

static gboolean
handle_set_wallpaper_uri (XdpDbusWallpaper *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_parent_window,
                          const char *arg_uri,
                          GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(SetWallpaperRequest) wallpaper_request = NULL;
  g_autoptr(DexFuture) future = NULL;

  g_debug ("Handle SetWallpaperURI");

  wallpaper_request = set_wallpaper_request_new (request,
                                                 arg_uri,
                                                 arg_parent_window,
                                                 arg_options);

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_wallpaper_complete_set_wallpaper_uri (object, invocation, request->id);

  future = dex_scheduler_spawn (NULL,
                                0,
                                handle_set_wallpaper,
                                wallpaper_request, NULL);
  future = dex_future_finally (future,
                               set_wallpaper_request_done,
                               g_steal_pointer (&wallpaper_request),
                               (GDestroyNotify) set_wallpaper_request_free);
  dex_future_disown (g_steal_pointer (&future));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_set_wallpaper_file (XdpDbusWallpaper *object,
                           GDBusMethodInvocation *invocation,
                           GUnixFDList *fd_list,
                           const char *arg_parent_window,
                           GVariant *arg_fd,
                           GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;
  int fd_id, fd;
  g_autoptr(DexFuture) future = NULL;
  g_autoptr(GError) error = NULL;

  g_autoptr(SetWallpaperRequest) wallpaper_request = NULL;

  g_debug ("Handle SetWallpaperFile");

  g_variant_get (arg_fd, "h", &fd_id);
  if (fd_id >= g_unix_fd_list_get_length (fd_list))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Bad file descriptor index");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  fd = g_unix_fd_list_get (fd_list, fd_id, &error);
  if (fd == -1)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  path = xdp_app_info_get_path_for_fd (request->app_info, fd, 0, NULL, NULL, &error);
  if (path == NULL)
    {
      g_debug ("Cannot get path for fd: %s", error->message);

      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  uri = g_filename_to_uri (path, NULL, NULL);

  wallpaper_request = set_wallpaper_request_new (request,
                                                 uri,
                                                 arg_parent_window,
                                                 arg_options);

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_dbus_wallpaper_complete_set_wallpaper_file (object, invocation, NULL, request->id);

  future = dex_scheduler_spawn (NULL,
                                0,
                                handle_set_wallpaper,
                                wallpaper_request, NULL);
  future = dex_future_finally (future,
                               set_wallpaper_request_done,
                               g_steal_pointer (&wallpaper_request),
                               (GDestroyNotify) set_wallpaper_request_free);
  dex_future_disown (g_steal_pointer (&future));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
wallpaper_iface_init (XdpDbusWallpaperIface *iface)
{
  iface->handle_set_wallpaper_uri = handle_set_wallpaper_uri;
  iface->handle_set_wallpaper_file = handle_set_wallpaper_file;
}

static void
wallpaper_init (Wallpaper *wallpaper)
{
  xdp_dbus_wallpaper_set_version (XDP_DBUS_WALLPAPER (wallpaper), 1);
}

static void
wallpaper_class_init (WallpaperClass *klass)
{
}

GDBusInterfaceSkeleton *
wallpaper_create (GDBusConnection *connection,
                  const char *dbus_name_access,
                  const char *dbus_name_wallpaper)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_dbus_impl_wallpaper_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 dbus_name_wallpaper,
                                                 DESKTOP_PORTAL_OBJECT_PATH,
                                                 NULL,
                                                 &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create wallpaper proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);
  wallpaper = g_object_new (wallpaper_get_type (), NULL);

  access_impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     dbus_name_access,
                                                     DESKTOP_PORTAL_OBJECT_PATH,
                                                     NULL,
                                                     &error);

  return G_DBUS_INTERFACE_SKELETON (wallpaper);
}
