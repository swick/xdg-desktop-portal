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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>

#include "entitlements.h"

#include "xdp-dbus.h"
#include "xdp-entitlements.h"
#include "xdp-impl-dbus.h"
#include "xdp-request.h"
#include "xdp-utils.h"

struct _PortalEntitlements
{
  XdpDbusEntitlementsSkeleton parent_instance;

  XdpDbusImplEntitlements *impl;
};

#define PORTAL_TYPE_ENTITLEMENTS (portal_entitlements_get_type ())
G_DECLARE_FINAL_TYPE (PortalEntitlements,
                      portal_entitlements,
                      PORTAL, ENTITLEMENTS,
                      XdpDbusEntitlementsSkeleton)

static void xdp_dbus_entitlements_iface_init (XdpDbusEntitlementsIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (PortalEntitlements,
                               portal_entitlements,
                               XDP_DBUS_TYPE_ENTITLEMENTS_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_ENTITLEMENTS,
                                                      xdp_dbus_entitlements_iface_init));

static void
enable_done (GObject      *source_object,
             GAsyncResult *result,
             gpointer      data)
{
  g_autoptr(XdpRequest) request = data;
  XdgDesktopPortalResponseEnum response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
  g_auto(GVariantBuilder) results_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  if (!xdp_dbus_impl_entitlements_call_enable_finish (
      XDP_DBUS_IMPL_ENTITLEMENTS (source_object),
      &response,
      &results,
      result,
      &error))
    {
      response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      g_debug ("Backend call failed: %s", error->message);
      goto out;
    }

out:
  if (request->exported)
    {
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&results_builder));
      xdp_request_unexport (request);
    }
}

static gboolean
handle_enable (XdpDbusEntitlements   *object,
               GDBusMethodInvocation *invocation,
               GVariant              *arg_entitlements,
               const char            *arg_parent_window,
               GVariant              *arg_options)
{
  PortalEntitlements *self = PORTAL_ENTITLEMENTS (object);
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(XdpEntitlements) entitlements = NULL;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (self->impl)),
    G_DBUS_PROXY_FLAGS_NONE,
    g_dbus_proxy_get_name (G_DBUS_PROXY (self->impl)),
    request->id,
    NULL,
    &error);

  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_set_impl_request (request, impl_request);
  xdp_request_export (request,
                      g_dbus_method_invocation_get_connection (invocation));

  {
    XdpEntitlements *declared;
    g_autoptr(XdpEntitlements) requested = NULL;

    declared = xdp_app_info_get_declared_entitlements (request->app_info);

    requested = xdp_entitlements_deserialize (arg_entitlements, &error);
    if (!requested)
      {
        g_dbus_method_invocation_return_error (invocation,
                                               XDG_DESKTOP_PORTAL_ERROR,
                                               XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                               "Invalid Entitlements: %s", error->message);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
      }

    if (declared)
      entitlements = xdp_entitlements_intersect (requested, declared);
  }

  xdp_dbus_entitlements_complete_enable (object, invocation, request->id);

  if (entitlements)
    {
      g_auto(GVariantBuilder) options_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      xdp_dbus_impl_entitlements_call_enable (self->impl,
        request->id,
        xdp_entitlements_serialize (entitlements),
        xdp_app_info_get_id (request->app_info),
        arg_parent_window,
        g_variant_builder_end (&options_builder),
        NULL,
        enable_done,
        g_object_ref (request));
    }
  else if (request->exported)
    {
      g_auto(GVariantBuilder) results_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS,
                                      g_variant_builder_end (&results_builder));
      xdp_request_unexport (request);
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_dbus_entitlements_iface_init (XdpDbusEntitlementsIface *iface)
{
  iface->handle_enable = handle_enable;
}

static void
portal_entitlements_dispose (GObject *object)
{
  PortalEntitlements *self = PORTAL_ENTITLEMENTS (object);

  g_clear_object (&self->impl);

  G_OBJECT_CLASS (portal_entitlements_parent_class)->dispose (object);
}

static void
portal_entitlements_class_init (PortalEntitlementsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = portal_entitlements_dispose;
}

static void
portal_entitlements_init (PortalEntitlements *self)
{
}

GDBusInterfaceSkeleton *
portal_entitlements_create (GDBusConnection *connection,
                            const char      *dbus_name)
{
  g_autoptr(PortalEntitlements) portal = NULL;
  g_autoptr(XdpDbusImplEntitlements) impl = NULL;
  unsigned int version;
  g_autoptr(GError) error = NULL;

  impl = xdp_dbus_impl_entitlements_proxy_new_sync (connection,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    dbus_name,
                                                    DESKTOP_PORTAL_OBJECT_PATH,
                                                    NULL,
                                                    &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create Entitlements proxy: %s", error->message);
      return NULL;
    }

  portal = g_object_new (PORTAL_TYPE_ENTITLEMENTS, NULL);
  portal->impl = g_steal_pointer (&impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  version = MIN (1, xdp_dbus_impl_entitlements_get_version (impl));
  xdp_dbus_entitlements_set_version (XDP_DBUS_ENTITLEMENTS (portal), version);

  return G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&portal));
}
