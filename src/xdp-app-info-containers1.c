/*
 * Copyright © 2024 Red Hat, Inc
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

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-login.h>
#endif
#include <fcntl.h>

#include "xdp-helper-dbus.h"

#include "xdp-app-info-containers1-private.h"

#define CONTAINERS1_XATTR           "user.xdg.containers1"
#define CONTAINERS1_XATTR_ENGINE    CONTAINERS1_XATTR ".engine"
#define CONTAINERS1_XATTR_APP       CONTAINERS1_XATTR ".appid"
#define CONTAINERS1_XATTR_INSTANCE  CONTAINERS1_XATTR ".instanceid"
#define CONTAINERS1_XATTR_PORTAL    CONTAINERS1_XATTR ".portalbus"

#define PORTAL_HELPER_OBJECT_PATH "/org/freedesktop/PortalHelper"

#define FLATPAK_ENGINE_ID "org.flatpak"

struct _XdpAppInfoContainers1
{
  XdpAppInfo parent;

  XdpDbusHelperPortalHelper *helper_proxy;
};

G_DEFINE_FINAL_TYPE (XdpAppInfoContainers1,
                     xdp_app_info_containers1,
                     XDP_TYPE_APP_INFO)

static GDBusConnection *session_bus;
/* bus name -> XdpDbusHelperPortalHelper */
GHashTable *helper_proxies;

static char *
xdp_app_info_containers1_remap_path (XdpAppInfo *app_info,
                                     const char *path)
{
  return g_strdup (path);
}

static void
xdp_app_info_containers1_dispose (GObject *object)
{
  //XdpAppInfoContainers1 *app_info = XDP_APP_INFO_CONTAINERS1 (object);

  G_OBJECT_CLASS (xdp_app_info_containers1_parent_class)->dispose (object);
}

static void
xdp_app_info_containers1_class_init (XdpAppInfoContainers1Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  XdpAppInfoClass *app_info_class = XDP_APP_INFO_CLASS (klass);

  object_class->dispose = xdp_app_info_containers1_dispose;

  app_info_class->remap_path = xdp_app_info_containers1_remap_path;
}

static void
xdp_app_info_containers1_init (XdpAppInfoContainers1 *app_info_containers1)
{
}

static gboolean
get_file_xattrs (int          fd,
                 const char  *attribute,
                 char       **value_out,
                 GError     **error)
{
  g_autoptr (GBytes) value = NULL;
  gconstpointer data;
  size_t size;
  g_autoptr (GString) str = NULL;

  value = xdp_fgetxattr_bytes (fd, attribute, error);
  if (!value)
    return FALSE;

  data = g_bytes_get_data (value, &size);

  if (!g_utf8_validate (data, size, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "xattr %s does not contain valid UTF-8", attribute);
      return FALSE;
    }

  str = g_string_new_len (data, size);
  *value_out = g_string_free_and_steal (g_steal_pointer (&str));
  return TRUE;
}

static gboolean
get_containers1_metadata (int      pid,
                          int      pidfd,
                          char   **engine_out,
                          char   **app_id_out,
                          char   **instance_id_out,
                          char   **portal_bus_out,
                          GError **error)
{
#ifdef HAVE_LIBSYSTEMD
  g_autoptr (GError) local_error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *cgroup_path = NULL;
  xdp_autofd int cgroup_root_fd = -1;
  xdp_autofd int fd = -1;
  g_autofree char *engine = NULL;
  g_autofree char *app_id = NULL;
  g_autofree char *instance_id = NULL;
  g_autofree char *portal_bus = NULL;

  path = g_strdup_printf ("/run/user/%d/systemd/private", getuid ());

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_set_error (error, XDP_APP_INFO_ERROR, XDP_APP_INFO_ERROR_WRONG_APP_KIND,
                   "Not a Containers1 process (missing xattr metadata on cgroup)");
      return FALSE;
    }

  if (sd_pidfd_get_cgroup (pidfd, &cgroup_path) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_file_error_from_errno (errno),
                   "Failed to get cgroup path");
      return FALSE;
    }

  if (!cgroup_path || !cgroup_path[0])
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Bad cgroup path");
      return FALSE;
    }

  if (!xdp_opendirat (AT_FDCWD, "/sys/fs/cgroup", TRUE, &cgroup_root_fd, error))
    return FALSE;

  /* the cgroup path starts with a / and we already checked that we can add 1 */
  if (!xdp_opendirat (cgroup_root_fd, cgroup_path + 1, TRUE, &fd, error))
    return FALSE;

  if (!get_file_xattrs (fd, CONTAINERS1_XATTR_ENGINE, &engine, &local_error) ||
      !get_file_xattrs (fd, CONTAINERS1_XATTR_APP, &app_id, &local_error) ||
      !get_file_xattrs (fd, CONTAINERS1_XATTR_INSTANCE, &instance_id, &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA))
        {
          g_set_error (error, XDP_APP_INFO_ERROR, XDP_APP_INFO_ERROR_WRONG_APP_KIND,
                       "Not a Containers1 process (missing xattr metadata on cgroup)");
          return FALSE;
        }

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  g_return_val_if_fail (engine != NULL, FALSE);
  g_return_val_if_fail (app_id != NULL, FALSE);
  g_return_val_if_fail (instance_id != NULL, FALSE);

  if (!get_file_xattrs (fd, CONTAINERS1_XATTR_PORTAL, &portal_bus, &local_error))
    {
      g_debug ("No PortalHelper bus name: %s", local_error->message);
      g_clear_error (&local_error);
    }

  if (!xdp_pidfd_verify_pid (pidfd, pid))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Client does not exist anymore");
      return FALSE;
    }

  *engine_out = g_steal_pointer (&engine);
  *app_id_out = g_steal_pointer (&app_id);
  *instance_id_out = g_steal_pointer (&instance_id);
  *portal_bus_out = g_steal_pointer (&portal_bus);
  return TRUE;
#else
  return flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED,
                             _("Cannot set cgroup xattrs: No systemd support built-in"));
#endif
}

static XdpDbusHelperPortalHelper *
get_helper_proxy (const char  *portal_bus,
                  GError     **error)
{
  g_autoptr (XdpDbusHelperPortalHelper) helper_proxy = NULL;
  XdpDbusHelperPortalHelper *cached_helper_proxy = NULL;

  if (!session_bus)
    session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);

  if (!session_bus)
    return NULL;

  if (!helper_proxies)
    {
      helper_proxies = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              (GDestroyNotify) g_free,
                                              (GDestroyNotify) g_object_unref);
    }

  cached_helper_proxy = g_hash_table_lookup (helper_proxies, portal_bus);
  if (cached_helper_proxy)
    return g_object_ref (cached_helper_proxy);

  helper_proxy =
    xdp_dbus_helper_portal_helper_proxy_new_sync (session_bus,
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  portal_bus,
                                                  PORTAL_HELPER_OBJECT_PATH,
                                                  NULL,
                                                  error);
  if (!helper_proxy)
    return NULL;

  g_hash_table_insert (helper_proxies,
                       g_strdup (portal_bus),
                       g_object_ref (helper_proxy));

  return g_steal_pointer (&helper_proxy);
}

XdpAppInfo *
xdp_app_info_containers1_new (int      pid,
                              int      pidfd,
                              GError **error)
{
  g_autoptr (XdpAppInfoContainers1) app_info_containers1 = NULL;
  g_autofree char *engine = NULL;
  g_autofree char *app_id = NULL;
  g_autofree char *instance_id = NULL;
  g_autofree char *portal_bus = NULL;
  g_autoptr(GAppInfo) gappinfo = NULL;
  g_autoptr (XdpDbusHelperPortalHelper) helper_proxy = NULL;

  if (pidfd < 0)
    {
      g_set_error (error, XDP_APP_INFO_ERROR, XDP_APP_INFO_ERROR_WRONG_APP_KIND,
                   "Cannot detect Containers1 because D-Bus provided no pidfd");
      return FALSE;
    }

  if (!get_containers1_metadata (pid, pidfd,
                                 &engine, &app_id, &instance_id, &portal_bus,
                                 error))
    return FALSE;

  if (portal_bus)
    {
      g_autoptr (GError) local_error = NULL;
      g_autoptr (GVariant) info = NULL;

      helper_proxy = get_helper_proxy (portal_bus, &local_error);

      if (xdp_dbus_helper_portal_helper_call_get_info_sync (helper_proxy,
                                                            app_id,
                                                            instance_id,
                                                            &info,
                                                            NULL,
                                                            &local_error))
        {
          const char *desktop_file = NULL;

          g_variant_lookup (info, "DesktopFile", "&s", &desktop_file);

          gappinfo = G_APP_INFO (g_desktop_app_info_new (desktop_file));
        }
      else
        {
          g_clear_object (&helper_proxy);
          g_warning ("Could not get app information: %s", local_error->message);
        }
    }

  app_info_containers1 = g_object_new (XDP_TYPE_APP_INFO_CONTAINERS1, NULL);
  app_info_containers1->helper_proxy = g_steal_pointer (&helper_proxy);
  xdp_app_info_initialize (XDP_APP_INFO (app_info_containers1),
                           engine, app_id, instance_id,
                           pidfd /* FIXME this can be wrong with xdg-dbus-proxy */,
                           gappinfo,
                           FALSE, TRUE, TRUE);

  return XDP_APP_INFO (g_steal_pointer (&app_info_containers1));
}
