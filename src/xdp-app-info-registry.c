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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "xdp-app-info.h"

#include "xdp-app-info-registry.h"

struct _XdpAppInfoRegistry
{
  GObject parent_instance;

  DexChannel *channel;
  GHashTable *app_infos; /* unique dbus name -> app info */
  GMutex app_infos_mutex;
};

G_DEFINE_FINAL_TYPE (XdpAppInfoRegistry,
                     xdp_app_info_registry,
                     G_TYPE_OBJECT)

typedef struct _GetAppInfoData
{
  GDBusMethodInvocation *invocation;
  DexPromise *promise;
} GetAppInfoData;

static void
get_app_info_data_free (GetAppInfoData *data)
{
  g_clear_object (&data->invocation);
  g_clear_pointer (&data->promise, dex_unref);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GetAppInfoData, get_app_info_data_free)

static DexFuture *
get_app_info_fiber (gpointer user_data)
{
  XdpAppInfoRegistry *registry = user_data;
  g_autoptr(DexChannel) channel = dex_ref (registry->channel);

  while (TRUE)
    {
      g_autoptr(GetAppInfoData) data = NULL;
      g_autoptr(XdpAppInfo) app_info = NULL;
      const char *sender;

      data = dex_await_pointer (dex_channel_receive (registry->channel), NULL);
      if (!data)
        break;

      sender = g_dbus_method_invocation_get_sender (data->invocation);
      app_info = xdp_app_info_registry_lookup_sender (registry, sender);

      if (!app_info)
        {
          g_autoptr(GError) error = NULL;

          /* TODO: Should convert to a future/fiber variant */
          app_info = xdp_app_info_new_for_invocation_sync (data->invocation,
                                                           NULL, &error);

          if (!app_info)
            {
              dex_promise_reject (data->promise, g_steal_pointer (&error));
              continue;
            }

          /* This is to allow xdp_app_info_registry_insert to work */
          {
            XdpAppInfo *existing_app_info = NULL;
            G_MUTEX_AUTO_LOCK (&registry->app_infos_mutex, locker);

            existing_app_info = g_hash_table_lookup (registry->app_infos, sender);
            if (!existing_app_info)
              {
                g_debug ("Adding XdpAppInfo: %s app '%s' for %s",
                         xdp_app_info_get_engine_display_name (app_info),
                         xdp_app_info_get_id (app_info),
                         sender);
                g_hash_table_insert (registry->app_infos,
                                     g_strdup (sender),
                                     g_object_ref (app_info));
              }
            else
              {
                g_debug ("Using already existing XdpAppInfo for %s", sender);
                g_set_object (&app_info, existing_app_info);
              }
          }
        }

      dex_promise_resolve_object (data->promise, g_steal_pointer (&app_info));
    }

  return dex_future_new_for_boolean (TRUE);
}

static void
xdp_app_info_registry_dispose (GObject *object)
{
  XdpAppInfoRegistry *registry = XDP_APP_INFO_REGISTRY (object);

  if (registry->channel)
    {
      dex_channel_close_send (registry->channel);
      g_clear_pointer (&registry->channel, dex_unref);
    }

  if (registry->app_infos)
    {
      g_mutex_clear (&registry->app_infos_mutex);
      g_clear_pointer (&registry->app_infos, g_hash_table_unref);
    }

  G_OBJECT_CLASS (xdp_app_info_registry_parent_class)->dispose (object);
}

static void
xdp_app_info_registry_class_init (XdpAppInfoRegistryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_app_info_registry_dispose;
}

static void
xdp_app_info_registry_init (XdpAppInfoRegistry *registry)
{
}

XdpAppInfoRegistry *
xdp_app_info_registry_new (void)
{
  XdpAppInfoRegistry *registry = g_object_new (XDP_TYPE_APP_INFO_REGISTRY, NULL);

  registry->channel = dex_channel_new (0);

  registry->app_infos = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free,
                                               g_object_unref);
  g_mutex_init (&registry->app_infos_mutex);

  dex_future_disown (dex_scheduler_spawn (NULL, 0,
                                          get_app_info_fiber,
                                          registry,
                                          NULL));

  return registry;
}

XdpAppInfo *
xdp_app_info_registry_lookup_sender (XdpAppInfoRegistry *registry,
                                     const char         *sender)
{
  XdpAppInfo *app_info = NULL;

  G_MUTEX_AUTO_LOCK (&registry->app_infos_mutex, locker);

  app_info = g_hash_table_lookup (registry->app_infos, sender);
  if (!app_info)
    return NULL;

  return g_object_ref (app_info);
}

gboolean
xdp_app_info_registry_has_sender (XdpAppInfoRegistry *registry,
                                  const char         *sender)
{
  G_MUTEX_AUTO_LOCK (&registry->app_infos_mutex, locker);

  return g_hash_table_contains (registry->app_infos, sender);
}

void
xdp_app_info_registry_insert (XdpAppInfoRegistry *registry,
                              XdpAppInfo         *app_info)
{
  const char *sender = xdp_app_info_get_sender (app_info);

  G_MUTEX_AUTO_LOCK (&registry->app_infos_mutex, locker);

  g_debug ("Adding XdpAppInfo: %s app '%s' for %s",
           xdp_app_info_get_engine_display_name (app_info),
           xdp_app_info_get_id (app_info),
           sender);

  g_hash_table_insert (registry->app_infos,
                       g_strdup (sender),
                       g_object_ref (app_info));
}

void
xdp_app_info_registry_delete (XdpAppInfoRegistry *registry,
                              const char         *sender)
{
  XdpAppInfo *app_info = NULL;

  G_MUTEX_AUTO_LOCK (&registry->app_infos_mutex, locker);

  app_info = g_hash_table_lookup (registry->app_infos, sender);
  if (!app_info)
    return;

  g_debug ("Deleting XdpAppInfo: %s app '%s' for %s",
           xdp_app_info_get_engine_display_name (app_info),
           xdp_app_info_get_id (app_info),
           sender);

  g_hash_table_remove (registry->app_infos, sender);
}

DexFuture *
xdp_app_info_registry_ensure_for_invocation_future (XdpAppInfoRegistry    *registry,
                                                    GDBusMethodInvocation *invocation)
{
  g_autoptr(DexPromise) promise = dex_promise_new ();
  g_autoptr(GetAppInfoData) data = NULL;

  if (!dex_channel_can_send (registry->channel))
    {
      dex_promise_reject (promise,
                          g_error_new (G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "Channel closed"));
      return DEX_FUTURE (g_steal_pointer (&promise));
    }

  data = g_new0 (GetAppInfoData, 1);
  data->invocation = g_object_ref (invocation);
  data->promise = dex_ref (promise);

  g_autoptr(DexFuture) f = NULL;
  f = dex_channel_send (registry->channel,
                                       dex_future_new_for_pointer (g_steal_pointer (&data)));

  return DEX_FUTURE (g_steal_pointer (&promise));
}
