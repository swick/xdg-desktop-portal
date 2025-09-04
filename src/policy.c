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

#if 0
entitlement
flatpak override --user --add-policy=portal.openuri.deeplink-origins=open.spotify.com org.gnome.clocks
portal.Policy.Ask('portal.openuri.deeplink-origins')
 -> Access impl -> permission store
portal.Policy.GrantedPolicies = ['portal.openuri.deeplink-origins']
portal.OpenURI... ()
 -> Desktop file has X-Flatpak -> check for portal.openuri.deeplink-origins in permission store
#endif

#include "config.h"

#include "policy.h"

#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

struct _XdpPolicyPortal
{
  XdpDbusPolicySkeleton parent_instance;

  XdpDbusImplAccess *access_impl;
};

#define XDP_TYPE_POLICY_PORTAL (xdp_policy_portal_get_type ())
G_DECLARE_FINAL_TYPE (XdpPolicyPortal,
                      xdp_policy_portal,
                      XDP, POLICY_PORTAL,
                      XdpDbusPolicySkeleton)

static void xdp_dbus_policy_iface_init (XdpDbusPolicyIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpPolicyPortal,
                               xdp_policy_portal,
                               XDP_DBUS_TYPE_POLICY_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_POLICY,
                                                      xdp_dbus_policy_iface_init));

static void
xdp_dbus_policy_iface_init (XdpDbusPolicyIface *iface)
{
  //iface->handle_request = handle_request;
}

static void
xdp_policy_portal_dispose (GObject *object)
{
  XdpPolicyPortal *self = XDP_POLICY_PORTAL (object);

  g_clear_object (&self->access_impl);

  G_OBJECT_CLASS (xdp_policy_portal_parent_class)->dispose (object);
}

static void
xdp_policy_portal_class_init (XdpPolicyPortalClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_policy_portal_dispose;
}

static void
xdp_policy_portal_init (XdpPolicyPortal *self)
{
}

GDBusInterfaceSkeleton *
xdp_policy_portal_create (GDBusConnection *connection,
                          const char      *access_dbus_name)
{
  g_autoptr(XdpPolicyPortal) policy_portal = NULL;
  g_autoptr(XdpDbusImplAccess) access_impl = NULL;
  g_autoptr(GError) error = NULL;

  access_impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     access_dbus_name,
                                                     DESKTOP_PORTAL_OBJECT_PATH,
                                                     NULL,
                                                     &error);
  if (access_impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (access_impl), G_MAXINT);

  policy_portal = g_object_new (XDP_TYPE_POLICY_PORTAL, NULL);
  policy_portal->access_impl = g_steal_pointer (&access_impl);

  return G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&policy_portal));
}
