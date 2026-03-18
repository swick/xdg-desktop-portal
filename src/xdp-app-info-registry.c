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

/*
 * ### DEX HELPER
 *
 */



typedef DexFuture *(* DexThreadInvokeFunc) (gpointer user_data);

typedef struct _InvocationData
{
  DexThreadInvokeFunc func;
  gpointer func_user_data;
  GDestroyNotify func_user_data_free;
} InvocationData;

static void
invocation_data_free (InvocationData *data)
{
  if (data->func_user_data_free)
    data->func_user_data_free (data->func_user_data);
  g_free (data);
}

static DexFuture *
dex_thread_invoke_in_context_idle_finally (DexFuture *future,
                                           gpointer   user_data)
{
  GTask *task = G_TASK (user_data);

  g_task_return_pointer (task, dex_ref (future), dex_unref);

  return dex_future_new_true ();
}

static gboolean
dex_thread_invoke_in_context_idle (gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  InvocationData *data;
  g_autoptr(DexFuture) future = NULL;

  data = g_task_get_task_data (task);

  future = data->func (data->func_user_data);
  future = dex_future_finally (future,
                               dex_thread_invoke_in_context_idle_finally,
                               g_object_ref (task),
                               g_object_unref);
  dex_future_disown (g_steal_pointer (&future));

  return G_SOURCE_REMOVE;
}

static DexFuture *
dex_thread_invoke_in_finish (GAsyncResult *result)
{
  return g_task_propagate_pointer (G_TASK (result), NULL);
}

static void
dex_thread_invoke_in_context_async (GMainContext        *main_context,
                                    DexThreadInvokeFunc  func,
                                    gpointer             func_user_data,
                                    GDestroyNotify       func_user_data_free,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  InvocationData *data;

  data = g_new0 (InvocationData, 1);
  data->func = func;
  data->func_user_data = func_user_data;
  data->func_user_data_free = func_user_data_free;

  task = g_task_new (NULL, NULL, callback, user_data);
  g_task_set_task_data (task, g_steal_pointer (&data),
                        (GDestroyNotify) invocation_data_free);

  g_main_context_invoke_full (main_context,
                              G_PRIORITY_DEFAULT,
                              dex_thread_invoke_in_context_idle,
                              g_steal_pointer (&task),
                              (GDestroyNotify) g_object_unref);
}

/*
 *  ### DEX HELPER DONE
 *
 */

struct _XdpAppInfoRegistry
{
  GObject parent_instance;

  GThread *thread_self;

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

  registry->thread_self = g_thread_self ();

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

  dex_future_disown (dex_channel_send (registry->channel,
                                       dex_future_new_for_pointer (g_steal_pointer (&data))));

  return DEX_FUTURE (g_steal_pointer (&promise));
}

typedef struct _EnsureAppInfoData
{
  XdpAppInfoRegistry *registry;
  GDBusMethodInvocation *invocation;
} EnsureAppInfoData;

static DexFuture *
ensure_app_info (gpointer user_data)
{
  EnsureAppInfoData *data = user_data;

  return xdp_app_info_registry_ensure_for_invocation_future (data->registry,
                                                             data->invocation);
}

static void
ensure_app_info_done_cb (GObject      *source_object,
                         GAsyncResult *res,
                         gpointer      data)
{
  DexFuture **future = data;

  *future = dex_thread_invoke_in_finish (res);
}


XdpAppInfo * xdp_app_info_registry_ensure_for_invocation_sync (XdpAppInfoRegistry     *registry,
                                                               GDBusMethodInvocation  *invocation,
                                                               GCancellable           *cancellable,
                                                               GError                **error)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  g_autoptr(DexFuture) future = NULL;
  g_autoptr(GMainContext) context = NULL;
  EnsureAppInfoData data;

  context = g_main_context_new ();
  g_main_context_push_thread_default (context);

  data.registry = registry;
  data.invocation = invocation;

  dex_thread_invoke_in_context_async (NULL,
                                      ensure_app_info,
                                      &data,
                                      NULL,
                                      ensure_app_info_done_cb,
                                      &future);

  while (future == NULL)
    g_main_context_iteration (context, TRUE);

  g_main_context_pop_thread_default (context);

  return dex_await_object (g_steal_pointer (&future), error);
}
