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

#include <stdint.h>
#include <glib.h>
#include <gio/gio.h>
#include <libdex.h>

#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

#include "xdp-dbus-wrappers.h"

static void
impl_request_proxy_new_cb (GObject      *object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  g_autoptr(DexPromise) promise = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;

  impl_request = xdp_dbus_impl_request_proxy_new_finish (result, &error);

  if (error == NULL)
    dex_promise_resolve_object (promise, g_steal_pointer (&impl_request));
  else
    dex_promise_reject (promise, g_steal_pointer (&error));
}

static DexFuture *
xdp_future_impl_request_proxy_new (GDBusConnection *connection,
                                   GDBusProxyFlags  flags,
                                   const gchar     *name,
                                   const gchar     *object_path)
{
  DexPromise *promise;

  promise = dex_promise_new_cancellable ();

  xdp_dbus_impl_request_proxy_new (connection,
                                   flags,
                                   name,
                                   object_path,
                                   dex_promise_get_cancellable (promise),
                                   impl_request_proxy_new_cb,
                                   dex_ref (promise));

  return DEX_FUTURE (promise);
}

XdpDbusImplRequest *
xdp_fiber_impl_request_proxy_new (GDBusConnection  *connection,
                                  GDBusProxyFlags   flags,
                                  const gchar      *name,
                                  const gchar      *object_path,
                                  GError          **error)
{
  return dex_await_object (xdp_future_impl_request_proxy_new (connection,
                                                              flags,
                                                              name,
                                                              object_path),
                           error);
}



static void
impl_wallpaper_call_set_wallpaper_uri_cb (GObject      *object,
                                          GAsyncResult *result,
                                          gpointer      user_data)
{
  g_autoptr(DexPromise) promise = user_data;
  g_autoptr(GError) error = NULL;
  guint response;

  if (xdp_dbus_impl_wallpaper_call_set_wallpaper_uri_finish (XDP_DBUS_IMPL_WALLPAPER (object),
                                                             &response,
                                                             result,
                                                             &error))
    dex_promise_resolve_uint (promise, response);
  else
    dex_promise_reject (promise, g_steal_pointer (&error));
}

static DexFuture *
xdp_future_impl_wallpaper_set_uri (XdpDbusImplWallpaper *proxy,
                                   const gchar          *arg_handle,
                                   const gchar          *arg_app_id,
                                   const gchar          *arg_parent_window,
                                   const gchar          *arg_uri,
                                   GVariant             *arg_options)
{
  DexPromise *promise;

  promise = dex_promise_new_cancellable ();
  xdp_dbus_impl_wallpaper_call_set_wallpaper_uri (proxy,
                                                  arg_handle,
                                                  arg_app_id,
                                                  arg_parent_window,
                                                  arg_uri,
                                                  arg_options,
                                                  dex_promise_get_cancellable (promise),
                                                  impl_wallpaper_call_set_wallpaper_uri_cb,
                                                  dex_ref (promise));

  return DEX_FUTURE (promise);
}

gboolean
xdp_fiber_impl_wallpaper_set_uri (XdpDbusImplWallpaper  *proxy,
                                  const gchar           *arg_handle,
                                  const gchar           *arg_app_id,
                                  const gchar           *arg_parent_window,
                                  const gchar           *arg_uri,
                                  GVariant              *arg_options,
                                  guint                 *out_response,
                                  GError               **error)
{
  guint response;
  g_autoptr(GError) local_error = NULL;

  response = dex_await_uint (xdp_future_impl_wallpaper_set_uri (proxy,
                                                                arg_handle,
                                                                arg_app_id,
                                                                arg_parent_window,
                                                                arg_uri,
                                                                arg_options),
                             &local_error);

  if (local_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (out_response)
    *out_response = response;

  return TRUE;
}

struct _XdpFutureWallpaperSkeletonClass
{
  XdpDbusWallpaperSkeletonClass parent_class;
};

static void wallpaper_iface_init (XdpDbusWallpaperIface *iface);

G_DEFINE_TYPE_WITH_CODE (XdpFutureWallpaperSkeleton,
                         xdp_future_wallpaper_skeleton,
                         XDP_DBUS_TYPE_WALLPAPER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_WALLPAPER,
                                                wallpaper_iface_init));

typedef struct _SetWallpaperUriData
{
  XdpDbusWallpaper *object;
  GDBusMethodInvocation *invocation;
  char *arg_parent_window;
  char *arg_uri;
  GVariant *arg_options;
} SetWallpaperUriData;

static void
set_wallpaper_uri_data_free (SetWallpaperUriData *d)
{
  g_clear_object (&d->object);
  g_clear_object (&d->invocation);
  g_clear_pointer (&d->arg_parent_window, g_free);
  g_clear_pointer (&d->arg_uri, g_free);
  g_clear_pointer (&d->arg_options, g_variant_unref);

  g_free (d);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SetWallpaperUriData,
                               set_wallpaper_uri_data_free)

G_DEFINE_INTERFACE (XdpFutureWallpaper, xdp_future_wallpaper, XDP_DBUS_TYPE_WALLPAPER)

static void
xdp_future_wallpaper_default_init (XdpFutureWallpaperInterface *iface)
{
}

static DexFuture*
handle_set_wallpaper_uri_future (gpointer user_data)
{
  SetWallpaperUriData *d = user_data;

  XDP_FUTURE_WALLPAPER_GET_IFACE (d->object)
    ->handle_set_wallpaper_uri (d->object,
                                d->invocation,
                                d->arg_parent_window,
                                d->arg_uri,
                                d->arg_options);

  return NULL;
}

static gboolean
handle_set_wallpaper_uri (XdpDbusWallpaper      *object,
                          GDBusMethodInvocation *invocation,
                          const char            *arg_parent_window,
                          const char            *arg_uri,
                          GVariant              *arg_options)
{
  g_autoptr(SetWallpaperUriData) data = NULL;
  g_autoptr(DexFuture) future = NULL;

  data = g_new0 (SetWallpaperUriData, 1);
  data->object = g_object_ref (object);
  data->invocation = g_object_ref (invocation);
  data->arg_parent_window = g_strdup (arg_parent_window);
  data->arg_uri = g_strdup (arg_uri);
  data->arg_options = g_variant_ref (arg_options);

  future = dex_scheduler_spawn (NULL,
                                0,
                                handle_set_wallpaper_uri_future,
                                data,
                                (GDestroyNotify) set_wallpaper_uri_data_free);
  dex_future_disown (g_steal_pointer (&future));

  return G_DBUS_METHOD_INVOCATION_UNHANDLED;
}

typedef struct _SetWallpaperFileData
{
  XdpDbusWallpaper *object;
  GDBusMethodInvocation *invocation;
  GUnixFDList *fd_list;
  char *arg_parent_window;
  GVariant *arg_fd;
  GVariant *arg_options;
} SetWallpaperFileData;

static void
set_wallpaper_file_data_free (SetWallpaperFileData *d)
{
  g_clear_object (&d->object);
  g_clear_object (&d->invocation);
  g_clear_object (&d->fd_list);
  g_clear_pointer (&d->arg_parent_window, g_free);
  g_clear_pointer (&d->arg_fd, g_variant_unref);
  g_clear_pointer (&d->arg_options, g_variant_unref);

  g_free (d);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SetWallpaperFileData,
                               set_wallpaper_file_data_free)

static DexFuture*
handle_set_wallpaper_file_future (gpointer user_data)
{
  SetWallpaperFileData *d = user_data;

  XDP_FUTURE_WALLPAPER_GET_IFACE (d->object)
    ->handle_set_wallpaper_file (d->object,
                                 d->invocation,
                                 d->fd_list,
                                 d->arg_parent_window,
                                 d->arg_fd,
                                 d->arg_options);

  return NULL;
}

static gboolean
handle_set_wallpaper_file (XdpDbusWallpaper      *object,
                           GDBusMethodInvocation *invocation,
                           GUnixFDList           *fd_list,
                           const char            *arg_parent_window,
                           GVariant              *arg_fd,
                           GVariant              *arg_options)
{
  g_autoptr(SetWallpaperFileData) data = NULL;
  g_autoptr(DexFuture) future = NULL;

  data = g_new0 (SetWallpaperFileData, 1);
  data->object = g_object_ref (object);
  data->invocation = g_object_ref (invocation);
  data->fd_list = g_object_ref (fd_list);
  data->arg_parent_window = g_strdup (arg_parent_window);
  data->arg_fd = g_variant_ref (arg_fd);
  data->arg_options = g_variant_ref (arg_options);

  future = dex_scheduler_spawn (NULL,
                                0,
                                handle_set_wallpaper_file_future,
                                data,
                                (GDestroyNotify) set_wallpaper_file_data_free);
  dex_future_disown (g_steal_pointer (&future));

  return G_DBUS_METHOD_INVOCATION_UNHANDLED;
}

static void
wallpaper_iface_init (XdpDbusWallpaperIface *iface)
{
  iface->handle_set_wallpaper_uri = handle_set_wallpaper_uri;
  iface->handle_set_wallpaper_file = handle_set_wallpaper_file;
}

static void
xdp_future_wallpaper_skeleton_init (XdpFutureWallpaperSkeleton *skeleton)
{
}

static void
xdp_future_wallpaper_skeleton_class_init (XdpFutureWallpaperSkeletonClass *klass)
{
}



#define XDP_TYPE_IMPL_REQUEST_RESULT (xdp_impl_request_result_get_type())
GType xdp_impl_request_result_get_type (void);

typedef struct _XdpImplRequestResult
{
  guint response;
  GVariant *results;
} XdpImplRequestResult;

static XdpImplRequestResult *
xdp_impl_request_result_new (guint     response,
                             GVariant *results)
{
  XdpImplRequestResult *r = g_new0 (XdpImplRequestResult, 1);
  r->response = response;
  r->results = g_variant_ref (results);

  return r;
}

static XdpImplRequestResult *
xdp_impl_request_result_copy (XdpImplRequestResult *r)
{
  return xdp_impl_request_result_new (r->response, r->results);
}

static void
xdp_impl_request_result_free (XdpImplRequestResult *r)
{
  g_clear_pointer (&r->results, g_variant_unref);
  free (r);
}

G_DEFINE_BOXED_TYPE (XdpImplRequestResult,
                     xdp_impl_request_result,
                     xdp_impl_request_result_copy,
                     xdp_impl_request_result_free)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpImplRequestResult,
                               xdp_impl_request_result_free)

static void
impl_access_call_access_dialog_cb (GObject      *object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  g_autoptr(DexPromise) promise = user_data;
  g_autoptr(GError) error = NULL;
  guint response;
  g_autoptr(GVariant) results = NULL;

  if (xdp_dbus_impl_access_call_access_dialog_finish (XDP_DBUS_IMPL_ACCESS (object),
                                                      &response,
                                                      &results,
                                                      result,
                                                      &error))
    {
      g_autoptr(XdpImplRequestResult) r =
        xdp_impl_request_result_new (response, results);

      dex_promise_resolve_boxed (promise,
                                 XDP_TYPE_IMPL_REQUEST_RESULT,
                                 g_steal_pointer (&r));
    }
  else
    {
      dex_promise_reject (promise, g_steal_pointer (&error));
    }
}

DexFuture *
xdp_future_impl_access_dialog (XdpDbusImplAccess *proxy,
                               const gchar       *arg_handle,
                               const gchar       *arg_app_id,
                               const gchar       *arg_parent_window,
                               const gchar       *arg_title,
                               const gchar       *arg_subtitle,
                               const gchar       *arg_body,
                               GVariant          *arg_options)
{
  DexPromise *promise;

  promise = dex_promise_new_cancellable ();
  xdp_dbus_impl_access_call_access_dialog (proxy,
                                           arg_handle,
                                           arg_app_id,
                                           arg_parent_window,
                                           arg_title,
                                           arg_subtitle,
                                           arg_body,
                                           arg_options,
                                           dex_promise_get_cancellable (promise),
                                           impl_access_call_access_dialog_cb,
                                           dex_ref (promise));

  return DEX_FUTURE (promise);
}

gboolean
xdp_fiber_impl_access_dialog (XdpDbusImplAccess  *proxy,
                              const gchar        *arg_handle,
                              const gchar        *arg_app_id,
                              const gchar        *arg_parent_window,
                              const gchar        *arg_title,
                              const gchar        *arg_subtitle,
                              const gchar        *arg_body,
                              GVariant           *arg_options,
                              guint              *out_response,
                              GVariant          **out_results,
                              GError            **error)
{
  g_autoptr(XdpImplRequestResult) r = NULL;

  r = dex_await_boxed (xdp_future_impl_access_dialog (
            proxy,
            arg_handle,
            arg_app_id,
            arg_parent_window,
            arg_title,
            arg_subtitle,
            arg_body,
            arg_options),
          error);

  if (!r)
    return FALSE;

  if (out_response)
    *out_response = r->response;
  if (out_results)
    *out_results = g_steal_pointer (&r->results);

  return TRUE;
}
