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

#include <string.h>
#include <glib/gi18n.h>

#include "policy.h"

#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-request.h"
#include "xdp-utils.h"

struct _XdpPolicyPortal
{
  XdpDbusPolicySkeleton parent_instance;

  XdpDbusImplAccess *access_impl;
  unsigned int supported_version;
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

typedef struct _SupportedPolicy
{
  const char *id;
  const char *default_description;
  unsigned int required_version;
} SupportedPolicy;

static SupportedPolicy supported_policies[] = {
  { "portal.test.policy", "Test Policy", 1 }, // FIXME _() doesnt work
};

static gboolean
filter_policies (XdpPolicyPortal  *self,
                 GVariant         *requested_policies,
                 GPtrArray        *policies,
                 GError          **error)
{
  GVariantIter policies_iter;
  const char *policy_id;

  if (g_variant_iter_init (&policies_iter, requested_policies) == 0)
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "No policies provided");
      return FALSE;
    }

    while (g_variant_iter_next (&policies_iter, "(&sa{sv})",
                                &policy_id,
                                NULL))
      {
        SupportedPolicy *policy = NULL;

        for (size_t i = 0; i < G_N_ELEMENTS (supported_policies); i++)
          {
            if (g_strcmp0 (policy_id, supported_policies[i].id) != 0)
              continue;

            policy = &supported_policies[i];
            break;
          }

        if (!policy)
          {
            g_debug ("Ignoring unknown policy %s", policy_id);
            continue;
          }

        if (policy->required_version > self->supported_version)
          {
            g_debug ("Ignoring policy %s because the impl does not support it",
                     policy_id);
            continue;
          }

        if (g_ptr_array_find (policies, policy, NULL))
          {
            g_debug ("Ignoring duplicate policy %s", policy_id);
            continue;
          }

        g_ptr_array_add (policies, policy);
      }

  return TRUE;
}

static void
ask_done (GObject      *source_object,
          GAsyncResult *result,
          gpointer      data)
{
}

static gboolean
handle_ask (XdpDbusPolicy         *object,
            GDBusMethodInvocation *invocation,
            GVariant              *arg_policies,
            const char            *arg_parent_window,
            GVariant              *arg_options)
{
  XdpPolicyPortal *self = XDP_POLICY_PORTAL (object);
  XdpRequest *request = xdp_request_from_invocation (invocation);
  g_autoptr(XdpDbusImplRequest) impl_request = NULL;
  g_autoptr(GPtrArray) policies = NULL;
  g_auto(GVariantBuilder) access_options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GError) error = NULL;

  g_debug ("[usb] Handling AccessDevices");

  REQUEST_AUTOLOCK (request);

  impl_request = xdp_dbus_impl_request_proxy_new_sync (
    g_dbus_proxy_get_connection (G_DBUS_PROXY (self->access_impl)),
    G_DBUS_PROXY_FLAGS_NONE,
    g_dbus_proxy_get_name (G_DBUS_PROXY (self->access_impl)),
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

  if (!filter_policies (self, arg_policies, policies, &error))
    {
        g_dbus_method_invocation_return_gerror (invocation, error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  {
    g_auto(GVariantBuilder) choices_builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(ssa(ss)s)"));

    for (size_t i = 0; i < policies->len; i++)
      {
        SupportedPolicy *policy = policies->pdata[i];

        g_variant_builder_add (&choices_builder, "{ssa(ss)s}",
                               policy->id,
                               policy->default_description,
                               NULL,
                               "false");
      }

    g_variant_builder_add (&access_options_builder, "{sv}",
                           "choices", g_variant_builder_end (&choices_builder));
    g_variant_builder_add (&access_options_builder, "{sv}",
                           "grant_label", g_variant_new_string (_("Grant")));
    g_variant_builder_add (&access_options_builder, "{sv}",
                           "deny_label", g_variant_new_string (_("Deny")));
  }

  xdp_dbus_policy_complete_ask (object, invocation, request->id);

  if (policies->len > 0)
    {
      xdp_dbus_impl_access_call_access_dialog (self->access_impl,
        request->id,
        xdp_app_info_get_id (request->app_info),
        arg_parent_window,
        _("TEST"), /* title */
        _("Test"), /* subtitle */
        _("test"), /* body */
        g_variant_builder_end (&access_options_builder),
        NULL,
        ask_done,
        g_object_ref (request));
    }
  else
    {
      // FIXME immediately finish request
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_dbus_policy_iface_init (XdpDbusPolicyIface *iface)
{
  iface->handle_ask = handle_ask;
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

  policy_portal->supported_version = 1;
  xdp_dbus_policy_set_version (XDP_DBUS_POLICY (policy_portal),
                               policy_portal->supported_version);

  return G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&policy_portal));
}
