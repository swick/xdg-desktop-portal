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

struct _XdpDesktopPortal
{
  GObject parent_instance;

  gboolean verbose;

  XdpPortalImpls *portal_impls;
};

G_DEFINE_FINAL_TYPE (XdpDesktopPortal, xdp_desktop_portal, G_TYPE_OBJECT)

static void
xdp_desktop_portal_dispose (GObject *object)
{
  XdpDesktopPortal *desktop_portal = XDP_DESKTOP_PORTAL (object);

  g_clear_object (&desktop_portal->portal_impls);

  G_OBJECT_CLASS (xdp_desktop_portal_parent_class)->dispose (object);
}

static void
xdp_desktop_portal_class_init (XdpDesktopPortalClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_desktop_portal_dispose;
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
    g_warning ("Support for %s::%s missing in %s",
               interface, method, G_STRLOC);

  return method_info ?  method_info->uses_request : TRUE;
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
                                             "Portal operation not allowed: %s", error->message);
      return FALSE;
    }

  if (method_needs_request (invocation))
    xdp_request_init_invocation (invocation, app_info);
  else
    xdp_call_init_invocation (invocation, app_info);

  return TRUE;
}

static void
export_portal_implementation (GDBusConnection *connection,
                              GDBusInterfaceSkeleton *skeleton)
{
  g_autoptr(GError) error = NULL;

  if (skeleton == NULL)
    {
      g_warning ("No skeleton to export");
      return;
    }

  g_dbus_interface_skeleton_set_flags (skeleton,
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_signal_connect (skeleton, "g-authorize-method",
                    G_CALLBACK (authorize_callback), NULL);

  if (!g_dbus_interface_skeleton_export (skeleton,
                                         connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Error: %s", error->message);
      return;
    }

  g_debug ("providing portal %s", g_dbus_interface_skeleton_get_info (skeleton)->name);
}

static void
export_host_portal_implementation (GDBusConnection        *connection,
                                   GDBusInterfaceSkeleton *skeleton)
{
  /* Host portal dbus method invocations run in the main thread without yielding
   * to the main loop. This means that any later method call of any portal will
   * see the effects of the host portal method call.
   *
   * This is important because the Registry modifies the XdpAppInfo and later
   * method calls must see the modified value.
   */

  g_autoptr(GError) error = NULL;

  if (skeleton == NULL)
    {
      g_warning ("No skeleton to export");
      return;
    }

  g_dbus_interface_skeleton_set_flags (skeleton,
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_NONE);

  if (!g_dbus_interface_skeleton_export (skeleton,
                                         connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Error: %s", error->message);
      return;
    }

  g_debug ("providing portal %s", g_dbus_interface_skeleton_get_info (skeleton)->name);
}

static void
on_peer_died (const char *name)
{
  close_requests_for_sender (name);
  close_sessions_for_sender (name);
  xdp_session_persistence_delete_transient_permissions_for_sender (name);
}

gboolean
xdp_desktop_portal_register (XdpDesktopPortal  *desktop_portal,
                             GDBusConnection   *connection,
                             GError           **error)
{
  XdpPortalImpls *portal_impls = desktop_portal->portal_impls;
  XdpPortalImplementation *implementation;
  XdpDbusImplLockdown *lockdown;
  XdpPortalImplementation *lockdown_impl;
  XdpPortalImplementation *access_impl;
  GQuark portal_errors G_GNUC_UNUSED;
  GPtrArray *impls;

  /* make sure errors are registered */
  portal_errors = XDG_DESKTOP_PORTAL_ERROR;

  xdp_connection_track_name_owners (connection, on_peer_died);

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

  lockdown_impl = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Lockdown");
  if (lockdown_impl != NULL)
    lockdown = xdp_dbus_impl_lockdown_proxy_new_sync (connection,
                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                      lockdown_impl->dbus_name,
                                                      DESKTOP_PORTAL_OBJECT_PATH,
                                                      NULL, NULL);

  if (lockdown == NULL)
    lockdown = xdp_dbus_impl_lockdown_skeleton_new ();

  export_portal_implementation (connection, memory_monitor_create (connection));
  export_portal_implementation (connection, power_profile_monitor_create (connection));
  export_portal_implementation (connection, network_monitor_create (connection));
  export_portal_implementation (connection, proxy_resolver_create (connection));
  export_portal_implementation (connection, trash_create (connection));
  export_portal_implementation (connection, game_mode_create (connection));
  export_portal_implementation (connection, realtime_create (connection));

  impls = xdp_portal_impls_find_all (portal_impls, "org.freedesktop.impl.portal.Settings");
  if (impls->len > 0)
    export_portal_implementation (connection, settings_create (connection, impls));
  g_ptr_array_free (impls, TRUE);

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.FileChooser");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  file_chooser_create (connection, implementation->dbus_name, lockdown));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.AppChooser");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  open_uri_create (connection, implementation->dbus_name, lockdown));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Print");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  print_create (connection, implementation->dbus_name, lockdown));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Notification");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  notification_create (connection, implementation->dbus_name));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Inhibit");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  inhibit_create (connection, implementation->dbus_name));

  access_impl = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Access");
  if (access_impl != NULL)
    {
      XdpPortalImplementation *tmp;

#ifdef HAVE_GEOCLUE
      export_portal_implementation (connection,
                                    location_create (connection,
                                                     access_impl->dbus_name,
                                                     lockdown));
#endif

      export_portal_implementation (connection,
                                    camera_create (connection,
                                                   access_impl->dbus_name,
                                                   lockdown));

      tmp = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Screenshot");
      if (tmp != NULL)
        export_portal_implementation (connection,
                                      screenshot_create (connection,
                                                         access_impl->dbus_name,
                                                         tmp->dbus_name));

      tmp = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Background");
      if (tmp != NULL)
        export_portal_implementation (connection,
                                      background_create (connection,
                                                         access_impl->dbus_name,
                                                         tmp->dbus_name));

      tmp = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Wallpaper");
      if (tmp != NULL)
        export_portal_implementation (connection,
                                      wallpaper_create (connection,
                                                        access_impl->dbus_name,
                                                        tmp->dbus_name));
    }

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Account");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  account_create (connection, implementation->dbus_name));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Email");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  email_create (connection, implementation->dbus_name));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Secret");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  secret_create (connection, implementation->dbus_name));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.GlobalShortcuts");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  global_shortcuts_create (connection, implementation->dbus_name));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.DynamicLauncher");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  dynamic_launcher_create (connection, implementation->dbus_name));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.ScreenCast");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  screen_cast_create (connection, implementation->dbus_name));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.RemoteDesktop");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  remote_desktop_create (connection, implementation->dbus_name));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Clipboard");
  if (implementation != NULL)
    export_portal_implementation (
        connection, clipboard_create (connection, implementation->dbus_name));

  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.InputCapture");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  input_capture_create (connection, implementation->dbus_name));

#ifdef HAVE_GUDEV
  implementation = xdp_portal_impls_find (portal_impls, "org.freedesktop.impl.portal.Usb");
  if (implementation != NULL)
    export_portal_implementation (connection,
                                  xdp_usb_create (connection, implementation->dbus_name));
#endif

  export_host_portal_implementation (connection, registry_create (connection));

  return TRUE;
}
