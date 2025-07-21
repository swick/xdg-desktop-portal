/*
 * Copyright © 2025 Red Hat, Inc
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "xdp-utils.h"
#include "xdp-call.h"
#include "xdp-dbus.h"
#include "xdp-documents.h"
#include "xdp-impl-dbus.h"
#include "xdp-method-info.h"
#include "xdp-portal-impl.h"
#include "xdp-session-persistence.h"

#include "account.h"
#include "background.h"
#include "camera.h"
#include "clipboard.h"
#include "dynamic-launcher.h"
#include "email.h"
#include "file-chooser.h"
#include "gamemode.h"
#include "global-shortcuts.h"
#include "inhibit.h"
#include "input-capture.h"
#include "location.h"
#include "memory-monitor.h"
#include "network-monitor.h"
#include "notification.h"
#include "open-uri.h"
#include "xdp-permissions.h"
#include "power-profile-monitor.h"
#include "print.h"
#include "proxy-resolver.h"
#include "realtime.h"
#include "registry.h"
#include "remote-desktop.h"
#include "xdp-request.h"
#include "screen-cast.h"
#include "screenshot.h"
#include "secret.h"
#include "settings.h"
#include "trash.h"
#include "usb.h"
#include "wallpaper.h"

#include "xdp-desktop-portal.h"

#define ACCESS_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Access"
#define LOCKDOWN_DBUS_IMPL_IFACE DESKTOP_DBUS_IMPL_IFACE ".Lockdown"

enum
{
  PEER_DIED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _XdpDesktopPortal
{
  GObject parent_instance;

  gboolean verbose;

  XdpPortalImpls *portal_impls;
  GDBusConnection *connection;

  XdpDbusImplLockdown *lockdown;
};

G_DEFINE_FINAL_TYPE (XdpDesktopPortal, xdp_desktop_portal, G_TYPE_OBJECT)

static void
xdp_desktop_portal_dispose (GObject *object)
{
  XdpDesktopPortal *desktop_portal = XDP_DESKTOP_PORTAL (object);

  g_clear_object (&desktop_portal->portal_impls);

  g_clear_object (&desktop_portal->lockdown);

  G_OBJECT_CLASS (xdp_desktop_portal_parent_class)->dispose (object);
}

static void
xdp_desktop_portal_class_init (XdpDesktopPortalClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_desktop_portal_dispose;

  signals[PEER_DIED] =
    g_signal_new ("peer-died",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
xdp_desktop_portal_init (XdpDesktopPortal *desktop_portal)
{
}

XdpDesktopPortal *
xdp_desktop_portal_new (gboolean opt_verbose)
{
  XdpDesktopPortal *desktop_portal =
    g_object_new (XDP_TYPE_DESKTOP_PORTAL, NULL);

  desktop_portal->verbose = opt_verbose;
  desktop_portal->portal_impls = xdp_portal_impls_new (desktop_portal);

  return desktop_portal;
}

GDBusConnection *
xdp_desktop_portal_get_connection (XdpDesktopPortal *desktop_portal)
{
  return desktop_portal->connection;
}

XdpPortalImpls *
xdp_desktop_portal_get_impls (XdpDesktopPortal *desktop_portal)
{
  return desktop_portal->portal_impls;
}

XdpDbusImplLockdown *
xdp_desktop_portal_get_lockdown_proxy (XdpDesktopPortal *desktop_portal)
{
  /* we share the lockdown proxy between portals */
  return g_object_ref (desktop_portal->lockdown);
}

XdpDbusImplAccess *
xdp_desktop_portal_get_access_proxy (XdpDesktopPortal *desktop_portal)
{
  XdpPortalImplementation *impl;
  XdpDbusImplAccess *access_impl;

  impl = xdp_portal_impls_find (desktop_portal->portal_impls,
                                ACCESS_DBUS_IMPL_IFACE);

  if (!impl)
    return NULL;

  access_impl =
    xdp_dbus_impl_access_proxy_new_sync (desktop_portal->connection,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         impl->dbus_name,
                                         DESKTOP_DBUS_PATH,
                                         NULL, NULL);

  if (access_impl)
    g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (access_impl), G_MAXINT);

  return access_impl;
}

gboolean
xdp_desktop_portal_is_verbose (XdpDesktopPortal *desktop_portal)
{
  return desktop_portal->verbose;
}

static gboolean
method_needs_request (GDBusMethodInvocation *invocation)
{
  const char *interface;
  const char *method;
  const XdpMethodInfo *method_info;

  interface = g_dbus_method_invocation_get_interface_name (invocation);
  method = g_dbus_method_invocation_get_method_name (invocation);

  method_info = xdp_method_info_find (interface, method);

  if (!method_info)
    {
      g_warning ("Support for %s::%s missing in %s",
                 interface, method, G_STRLOC);
    }

  return method_info ? method_info->uses_request : TRUE;
}

static gboolean
authorize_callback (GDBusInterfaceSkeleton *interface,
                    GDBusMethodInvocation  *invocation,
                    gpointer                user_data)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(GError) error = NULL;

  app_info = xdp_invocation_ensure_app_info_sync (invocation, NULL, &error);
  if (app_info == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: %s",
                                             error->message);
      return FALSE;
    }

  if (method_needs_request (invocation))
    xdp_request_init_invocation (invocation, app_info);
  else
    xdp_call_init_invocation (invocation, app_info);

  return TRUE;
}

gboolean
xdp_desktop_portal_export (XdpDesktopPortal        *desktop_portal,
                           GDBusInterfaceSkeleton  *skeleton,
                           GError                 **error)
{
  g_return_val_if_fail (G_IS_DBUS_INTERFACE_SKELETON (skeleton), FALSE);

  g_dbus_interface_skeleton_set_flags (
    skeleton,
    G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

  g_signal_connect (skeleton, "g-authorize-method",
                    G_CALLBACK (authorize_callback), NULL);

  return g_dbus_interface_skeleton_export (skeleton,
                                           desktop_portal->connection,
                                           DESKTOP_DBUS_PATH,
                                           error);
}

gboolean
xdp_desktop_portal_export_host (XdpDesktopPortal        *desktop_portal,
                                GDBusInterfaceSkeleton  *skeleton,
                                GError                 **error)
{
  /* Host portal dbus method invocations run in the main thread without yielding
   * to the main loop. This means that any later method call of any portal will
   * see the effects of the host portal method call.
   *
   * This is important because the Registry modifies the XdpAppInfo and later
   * method calls must see the modified value.
   */

  g_return_val_if_fail (G_IS_DBUS_INTERFACE_SKELETON (skeleton), FALSE);

  g_dbus_interface_skeleton_set_flags (skeleton,
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_NONE);

  return g_dbus_interface_skeleton_export (skeleton,
                                           desktop_portal->connection,
                                           DESKTOP_DBUS_PATH,
                                           error);
}

static void
export_portals (XdpDesktopPortal *desktop_portal)
{
  memory_monitor_create (desktop_portal);
  power_profile_monitor_create (desktop_portal);
  network_monitor_create (desktop_portal);
  proxy_resolver_create (desktop_portal);
  trash_create (desktop_portal);
  game_mode_create (desktop_portal);
  realtime_create (desktop_portal);
  settings_create (desktop_portal);
  file_chooser_create (desktop_portal);
  open_uri_create (desktop_portal);
  print_create (desktop_portal);
  notification_create (desktop_portal);
  inhibit_create (desktop_portal);
#ifdef HAVE_GEOCLUE
  location_create (desktop_portal);
#endif
  camera_create (desktop_portal);
  screenshot_create (desktop_portal);
  background_create (desktop_portal);
  wallpaper_create (desktop_portal);
  account_create (desktop_portal);
  email_create (desktop_portal);
  secret_create (desktop_portal);
  global_shortcuts_create (desktop_portal);
  dynamic_launcher_create (desktop_portal);
  screen_cast_create (desktop_portal);
  remote_desktop_create (desktop_portal);
  clipboard_create (desktop_portal);
  input_capture_create (desktop_portal);
#ifdef HAVE_GUDEV
  xdp_usb_create (desktop_portal);
#endif
  registry_create (desktop_portal);
}

static void
on_peer_died (XdpDesktopPortal *desktop_portal,
              const char       *name,
              gpointer          user_data)
{
  close_requests_for_sender (name);
  close_sessions_for_sender (name);
  xdp_session_persistence_delete_transient_permissions_for_sender (name);
  xdp_app_info_delete_for_sender (name);
}

static void
on_name_owner_changed (GDBusConnection *connection,
                       const gchar     *sender_name,
                       const gchar     *object_path,
                       const gchar     *interface_name,
                       const gchar     *signal_name,
                       GVariant        *parameters,
                       gpointer         user_data)
{
  XdpDesktopPortal *desktop_portal = user_data;
  const char *name, *from, *to;

  g_variant_get (parameters, "(&s&s&s)", &name, &from, &to);

  if (name[0] != ':' ||
      strcmp (name, from) != 0 ||
      strcmp (to, "") != 0)
    return;

  g_signal_emit (desktop_portal, signals[PEER_DIED], 0, name);
}

static void
track_name_owners (XdpDesktopPortal *desktop_portal)
{
  g_dbus_connection_signal_subscribe (desktop_portal->connection,
                                      DBUS_DBUS_NAME,
                                      DBUS_DBUS_IFACE,
                                      "NameOwnerChanged",
                                      DBUS_DBUS_PATH,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      on_name_owner_changed,
                                      desktop_portal, NULL);
}

gboolean
xdp_desktop_portal_register (XdpDesktopPortal  *desktop_portal,
                             GDBusConnection   *connection,
                             GError           **error)
{
  XdpPortalImpls *portal_impls = desktop_portal->portal_impls;
  XdpPortalImplementation *lockdown_impl;
  GQuark portal_errors G_GNUC_UNUSED;

  desktop_portal->connection = connection;

  /* make sure errors are registered */
  portal_errors = XDG_DESKTOP_PORTAL_ERROR;

  track_name_owners (desktop_portal);
  g_signal_connect (desktop_portal, "peer-died",
                    G_CALLBACK (on_peer_died),
                    NULL);

  if (!xdp_init_permission_store (connection, error))
    {
      g_prefix_error_literal (error, "No permission store: ");
      return FALSE;
    }

  if (!xdp_init_document_proxy (connection, error))
    {
      g_prefix_error_literal (error, "No document portal: ");
      return FALSE;
    }

  lockdown_impl = xdp_portal_impls_find (portal_impls, LOCKDOWN_DBUS_IMPL_IFACE);
  if (lockdown_impl)
    {
      desktop_portal->lockdown =
          xdp_dbus_impl_lockdown_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 lockdown_impl->dbus_name,
                                                 DESKTOP_DBUS_PATH,
                                                 NULL, NULL);
    }

  if (!desktop_portal->lockdown)
    desktop_portal->lockdown = xdp_dbus_impl_lockdown_skeleton_new ();

  export_portals (desktop_portal);

  return TRUE;
}
