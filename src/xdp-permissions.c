/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <string.h>

#include "xdp-permissions.h"

static XdpDbusImplPermissionStore *permission_store = NULL;

char **
xdp_get_permissions_sync (const char *app_id,
                          const char *table,
                          const char *id)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  g_autofree char **permissions = NULL;

  if (!xdp_dbus_impl_permission_store_call_lookup_sync (permission_store,
                                                        table,
                                                        id,
                                                        &out_perms,
                                                        &out_data,
                                                        NULL,
                                                        &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("No '%s' permissions found: %s", table, error->message);
      return NULL;
    }

  if (!g_variant_lookup (out_perms, app_id, "^a&s", &permissions))
    {
      g_debug ("No permissions stored for: %s %s, app %s", table, id, app_id);

      return NULL;
    }

  return g_strdupv (permissions);
}

XdpPermission
xdp_permissions_to_tristate (char **permissions)
{
  if (g_strv_length ((char **)permissions) != 1)
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
      return XDP_PERMISSION_UNSET;
    }

  if (strcmp (permissions[0], "yes") == 0)
    return XDP_PERMISSION_YES;
  else if (strcmp (permissions[0], "no") == 0)
    return XDP_PERMISSION_NO;
  else if (strcmp (permissions[0], "ask") == 0)
    return XDP_PERMISSION_ASK;
  else
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
    }

  return XDP_PERMISSION_UNSET;
}

char **
xdp_permissions_from_tristate (XdpPermission permission)
{
  char *permission_str;
  char **permissions;

  switch (permission)
    {
    case XDP_PERMISSION_UNSET:
      return NULL;
    case XDP_PERMISSION_NO:
      permission_str = g_strdup ("no");
      break;
    case XDP_PERMISSION_YES:
      permission_str = g_strdup ("yes");
      break;
    case XDP_PERMISSION_ASK:
      permission_str = g_strdup ("ask");
      break;
    default:
      g_assert_not_reached ();
      break;
    }

  permissions = g_new0 (char *, 2);
  permissions[0] = permission_str;

  return permissions;
}

void
xdp_set_permissions_sync (const char         *app_id,
                          const char         *table,
                          const char         *id,
                          const char * const *permissions)
{
  g_autoptr(GError) error = NULL;

  if (!xdp_dbus_impl_permission_store_call_set_permission_sync (permission_store,
                                                                table,
                                                                TRUE,
                                                                id,
                                                                app_id,
                                                                permissions,
                                                                NULL,
                                                                &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Error updating permission store: %s", error->message);
    }
}

XdpPermission
xdp_get_permission_sync (const char *app_id,
                         const char *table,
                         const char *id)
{
  g_auto(GStrv) perms = NULL;

  perms = xdp_get_permissions_sync (app_id, table, id);
  if (perms)
    return xdp_permissions_to_tristate (perms);

  return XDP_PERMISSION_UNSET;
}

void
xdp_set_permission_sync (const char    *app_id,
                         const char    *table,
                         const char    *id,
                         XdpPermission  permission)
{
  g_auto(GStrv) perms = NULL;

  perms = xdp_permissions_from_tristate (permission);
  xdp_set_permissions_sync (app_id, table, id, (const char * const *)perms);
}

gboolean
xdp_init_permission_store (GDBusConnection  *connection,
                           GError          **error)
{
  permission_store = xdp_dbus_impl_permission_store_proxy_new_sync (connection,
                                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                                    "org.freedesktop.impl.portal.PermissionStore",
                                                                    "/org/freedesktop/impl/portal/PermissionStore",
                                                                    NULL, error);
  return (permission_store != NULL);
}

XdpDbusImplPermissionStore *
xdp_get_permission_store (void)
{
  return permission_store;
}

typedef struct _XdpGetPermissionData {
  DexPromise *promise;
  char *app_id;
  char *table;
  char *id;
} XdpGetPermissionData;

static void
xdp_get_permission_data_free (XdpGetPermissionData *data)
{
  dex_clear (&data->promise);
  g_clear_pointer (&data->app_id, g_free);
  g_clear_pointer (&data->table, g_free);
  g_clear_pointer (&data->id, g_free);

  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpGetPermissionData, xdp_get_permission_data_free)

static void
xdp_get_permission_cb (GObject      *object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr(XdpGetPermissionData) data = user_data;
  DexPromise *promise = data->promise;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char **permissions = NULL;

  if (!xdp_dbus_impl_permission_store_call_lookup_finish (permission_store,
                                                          &out_perms,
                                                          &out_data,
                                                          result,
                                                          &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("No '%s' permissions found: %s", data->table, error->message);

      dex_promise_resolve_uint (promise, XDP_PERMISSION_UNSET);
      return;
    }

  if (!g_variant_lookup (out_perms, data->app_id, "^a&s", &permissions))
    {
      g_debug ("No permissions stored for: %s %s, app %s",
               data->table, data->id, data->app_id);

      dex_promise_resolve_uint (promise, XDP_PERMISSION_UNSET);
      return;
    }

  dex_promise_resolve_uint (promise, xdp_permissions_to_tristate (permissions));
}

DexFuture *
xdp_future_get_permission (const char *app_id,
                           const char *table,
                           const char *id)
{
  DexPromise *promise;
  XdpGetPermissionData *data;

  promise = dex_promise_new_cancellable ();

  data = g_new0 (XdpGetPermissionData, 1);
  data->promise = dex_ref (promise);
  data->app_id = g_strdup (app_id);
  data->table = g_strdup (table);
  data->id = g_strdup (id);

  xdp_dbus_impl_permission_store_call_lookup (permission_store,
                                              table,
                                              id,
                                              dex_promise_get_cancellable (promise),
                                              xdp_get_permission_cb,
                                              data);

  return DEX_FUTURE (promise);
}

XdpPermission
xdp_fiber_get_permission (const char *app_id,
                          const char *table,
                          const char *id)
{
  return dex_await_uint (xdp_future_get_permission (app_id, table, id), NULL);
}
