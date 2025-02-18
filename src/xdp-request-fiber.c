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

#include "xdp-dbus-wrappers.h"

#include "xdp-request-fiber.h"


struct _XdpRequestFiber
{
  XdpDbusRequestSkeleton parent_instance;

  GDBusMethodInvocation *invocation;
  XdpAppInfo *app_info;
  char *path;
  XdpDbusImplRequest *impl_request;
  gboolean connection_closed;
  gboolean exported;
  XdpRequestResponse response;
  GVariant *results;
};

static void xdp_dbus_request_iface_init (XdpDbusRequestIface *iface);

G_DEFINE_TYPE_WITH_CODE (XdpRequestFiber,
                         xdp_request_fiber,
                         XDP_DBUS_TYPE_REQUEST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_REQUEST,
                                                xdp_dbus_request_iface_init))

static GHashTable *
get_existing_requests (void)
{
  static GHashTable *requests = NULL;

  if (g_once_init_enter (&requests))
    {
      GHashTable *h = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, NULL);

      g_once_init_leave (&requests, h);
    }

  return requests;
}

static char *
make_request_path (const char *sender,
                   const char *token)
{
  GHashTable *requests = get_existing_requests ();
  g_autofree char *sender_path = NULL;
  g_autofree char *id = NULL;

  sender_path = g_strdup (sender + 1);
  for (size_t i = 0; sender_path[i]; i++)
    if (sender_path[i] == '.')
      sender_path[i] = '_';

  id = g_strdup_printf ("/org/freedesktop/portal/desktop/request/%s/%s",
                        sender_path,
                        token);

  while (g_hash_table_contains (requests, id))
    {
      uint32_t random = g_random_int ();

      g_clear_pointer (&id, g_free);
      id = g_strdup_printf ("/org/freedesktop/portal/desktop/request/%s/%s/%u",
                            sender_path,
                            token,
                            random);
    }

  g_hash_table_add (requests, g_strdup (id));

  return g_steal_pointer (&id);
}

static void
release_request_path (char *path)
{
  GHashTable *requests = get_existing_requests ();

  if (!path)
    return;

  g_hash_table_remove (requests, path);
  free (path);
}

const char *
xdp_request_fiber_get_path (XdpRequestFiber *request)
{
  return request->path;
}

void
xdp_request_fiber_set_response (XdpRequestFiber    *request,
                                XdpRequestResponse  response,
                                GVariant           *results)
{
  request->response = response;

  g_clear_pointer (&request->results, g_variant_unref);
  if (results)
    request->results = g_variant_ref_sink (results);
}

static gboolean
on_authorize_method (GDBusInterfaceSkeleton *interface,
                     GDBusMethodInvocation  *invocation,
                     gpointer                user_data)
{
  XdpRequestFiber *request = XDP_REQUEST_FIBER (user_data);
  const char *request_sender =
    g_dbus_method_invocation_get_sender (request->invocation);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  if (g_strcmp0 (sender, request_sender) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: Unmatched caller");
      return FALSE;
    }

  return TRUE;
}

static void
xdp_request_fiber_unexport (XdpRequestFiber *request)
{
  g_autoptr(GVariant) results = NULL;

  if (!request->exported)
    return;

  if (request->results)
    {
      results = g_steal_pointer (&request->results);
    }
  else
    {
      g_auto(GVariantBuilder) opt_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      results = g_variant_ref_sink (g_variant_builder_end (&opt_builder));
    }

  g_debug ("sending response: %d", request->response);
  xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                  request->response,
                                  results);

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  request->exported = FALSE;
}

static void
xdp_request_fiber_close_immediate (XdpRequestFiber *request)
{
  xdp_request_fiber_unexport (request);

  if (request->impl_request)
    {
      xdp_dbus_impl_request_call_close (request->impl_request,
                                        NULL, NULL, NULL);
      g_clear_object (&request->impl_request);
    }
}

gboolean
xdp_request_fiber_close (XdpRequestFiber  *request,
                         GError          **error)
{
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  xdp_request_fiber_unexport (request);

  if (request->impl_request)
    {
      impl_request = g_steal_pointer (&request->impl_request);
      return xdp_fiber_impl_request_close (impl_request, error);
    }

  return TRUE;
}

static void
on_connection_closed (GDBusConnection *self,
                      gboolean         remote_peer_vanished,
                      GError          *error,
                      gpointer         user_data)
{
  XdpRequestFiber *request = XDP_REQUEST_FIBER (user_data);

  request->connection_closed = TRUE;
  xdp_request_fiber_close_immediate (request);
}

static void
xdp_request_fiber_response (XdpDbusRequest *object,
                            guint           arg_response,
                            GVariant       *arg_results)
{
  XdpRequestFiber *request = XDP_REQUEST_FIBER (object);
  GDBusInterfaceSkeleton *skeleton = G_DBUS_INTERFACE_SKELETON (object);
  g_autolist(GDBusConnection) connections = NULL;
  g_autoptr(GVariant) signal_variant = NULL;
  const char *sender;

  sender = g_dbus_method_invocation_get_sender (request->invocation);

  connections = g_dbus_interface_skeleton_get_connections (skeleton);

  signal_variant = g_variant_ref_sink (g_variant_new ("(u@a{sv})",
                                                      arg_response,
                                                      arg_results));

  for (GList *l = connections; l != NULL; l = l->next)
    {
      GDBusConnection *connection = l->data;
      g_dbus_connection_emit_signal (connection,
                                     sender,
                                     g_dbus_interface_skeleton_get_object_path (skeleton),
                                     "org.freedesktop.portal.Request",
                                     "Response",
                                     signal_variant,
                                     NULL);
    }
}

static gboolean
xdp_request_fiber_handle_close (XdpDbusRequest        *object,
                                GDBusMethodInvocation *invocation)
{
  XdpRequestFiber *request = XDP_REQUEST_FIBER (object);
  g_autoptr(GError) error = NULL;

  g_debug ("Handling Close");

  if (!xdp_request_fiber_close (request, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_request_complete_close (XDP_DBUS_REQUEST (request), invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_dbus_request_iface_init (XdpDbusRequestIface *iface)
{
  iface->handle_close = xdp_request_fiber_handle_close;
  iface->response = xdp_request_fiber_response;
}

static void
xdp_request_fiber_init (XdpRequestFiber *request)
{
}

static void
xdp_request_fiber_dispose (GObject *object)
{
  XdpRequestFiber *request = XDP_REQUEST_FIBER (object);

  xdp_request_fiber_close_immediate (request);

  g_clear_object (&request->invocation);
  g_clear_object (&request->app_info);
  g_clear_object (&request->impl_request);
  g_clear_pointer (&request->path, release_request_path);
  g_clear_pointer (&request->results, g_variant_unref);

  G_OBJECT_CLASS (xdp_request_fiber_parent_class)->dispose (object);
}

static void
xdp_request_fiber_class_init (XdpRequestFiberClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose  = xdp_request_fiber_dispose;
}

XdpRequestFiber *
xdp_request_fiber_new (GDBusMethodInvocation  *invocation,
                       XdpAppInfo             *app_info,
                       const char             *token,
                       const char             *dbus_name,
                       GError                **error)
{
  g_autoptr(XdpRequestFiber) request = NULL;
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  request = g_object_new (XDP_TYPE_REQUEST_FIBER, NULL);
  request->invocation = g_object_ref (invocation);
  request->app_info = g_object_ref (app_info);
  request->path = make_request_path (sender, token);
  request->response = XDP_REQUEST_RESPONSE_OTHER;

  g_signal_connect_object (connection,
                           "closed",
                           G_CALLBACK (on_connection_closed),
                           request,
                           G_CONNECT_DEFAULT);

  g_signal_connect_object (request, "g-authorize-method",
                           G_CALLBACK (on_authorize_method),
                           request,
                           G_CONNECT_DEFAULT);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         connection,
                                         request->path,
                                         error))
    return NULL;

  request->exported = TRUE;

  request->impl_request =
    xdp_fiber_impl_request_proxy_new (connection,
                                      G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                      dbus_name,
                                      request->path,
                                      error);
  if (!request->impl_request)
    return NULL;

  if (request->connection_closed)
    xdp_request_fiber_close_immediate (request);

  return g_steal_pointer (&request);
}

XdpRequestFiber *
xdp_request_fiber_new_from_options (GDBusMethodInvocation  *invocation,
                                    XdpAppInfo             *app_info,
                                    GVariant               *options,
                                    const char             *dbus_name,
                                    GError                **error)
{
  const char *token = NULL;

  if (options)
    g_variant_lookup (options, "handle_token", "&s", &token);

  return xdp_request_fiber_new (invocation,
                                app_info,
                                token ? token : "t",
                                dbus_name,
                                error);
}
