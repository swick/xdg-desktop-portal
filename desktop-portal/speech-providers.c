/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "speech-providers.h"

#include <gio/gdesktopappinfo.h>

#include "speech-provider-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"

#define PROVIDER_INTERFACE "org.freedesktop.Speech.Provider"
#define VOICE_CACHE_TABLE "speech-voices"
#define PROVIDER_INIT_MAX_DELAY_MSEC 2000

typedef struct _XdpVoiceProvider
{
  GObject parent;

  char                *bus_name;     /* (owned) */
  char                *obj_path;     /* (owned) */
  GDBusConnection     *connection;   /* (unowned) */
  GCancellable        *cancellable;  /* (owned) */
  SpeechProviderProxy *proxy;        /* (owned) (nullable) */
  GVariant            *voices;       /* (owned) (nullable) */
} XdpVoiceProvider;

G_DEFINE_FINAL_TYPE (XdpVoiceProvider,
                     xdp_voice_provider,
                     G_TYPE_OBJECT)

enum
{
  VOICES_CHANGED,
  N_PROVIDER_SIGNALS,
};

static unsigned int provider_signals[N_PROVIDER_SIGNALS] = { 0 };

const char *
xdp_voice_provider_get_bus_name (XdpVoiceProvider *self)
{
  return self->bus_name;
}

GVariant *
xdp_voice_provider_get_voices (XdpVoiceProvider *self)
{
  return self->voices;
}

const char *
xdp_voice_provider_get_name (XdpVoiceProvider *self)
{
  if (!self->proxy)
    return NULL;

  return speech_provider_proxy_get_name (self->proxy);
}

gboolean
xdp_voice_provider_is_ready (XdpVoiceProvider *self)
{
  return self->proxy != NULL;
}

DexFuture *
xdp_voice_provider_synthesize (XdpVoiceProvider *self,
                               GVariant         *pipe_fd,
                               const char       *text,
                               const char       *voice_id,
                               double            pitch,
                               double            rate,
                               gboolean          is_ssml,
                               const char       *language,
                               GUnixFDList      *fd_list)
{
  return speech_provider_proxy_call_synthesize_future (self->proxy,
                                                       pipe_fd, text, voice_id,
                                                       pitch, rate, is_ssml,
                                                       language,
                                                       G_DBUS_CALL_FLAGS_NONE,
                                                       -1, fd_list);
}

static void
cache_provider_voices (const char *bus_name,
                       GVariant   *voices)
{
  XdpDbusImplPermissionStore *store = xdp_get_permission_store ();

  if (!store)
    return;

  dex_future_disown (
      xdp_dbus_impl_permission_store_call_set_value_future (
          store,
          VOICE_CACHE_TABLE,
          TRUE,
          bus_name,
          g_variant_new_variant (voices)));
}

static GVariant *
lookup_cached_voices (const char *bus_name)
{
  XdpDbusImplPermissionStore *store = xdp_get_permission_store ();
  g_autoptr (XdpDbusImplPermissionStoreLookupResult) result = NULL;
  g_autoptr (GVariant) data = NULL;

  if (!store)
    return NULL;

  result = dex_await_boxed (
      xdp_dbus_impl_permission_store_call_lookup_future (
          store,
          VOICE_CACHE_TABLE,
          bus_name),
      NULL);
  if (!result)
    return NULL;

  data = g_variant_get_variant (result->data);
  if (!data)
    return NULL;

  return g_variant_ref (data);
}

static void
on_voices_notify (SpeechProviderProxy *proxy,
                  GParamSpec          *pspec,
                  XdpVoiceProvider    *self)
{
  GVariant *voices = speech_provider_proxy_get_voices (proxy);

  if (voices)
    {
      g_clear_pointer (&self->voices, g_variant_unref);
      self->voices = g_variant_ref (voices);
      cache_provider_voices (self->bus_name, voices);
    }

  g_signal_emit (self, provider_signals[VOICES_CHANGED], 0);
}

static DexFuture *
init_provider_fiber (gpointer user_data)
{
  XdpVoiceProvider *self = XDP_VOICE_PROVIDER (user_data);
  g_autoptr (GVariant) cached_voices = NULL;
  g_autoptr (GError) error = NULL;
  GVariant *voices;

  cached_voices = lookup_cached_voices (self->bus_name);

  if (!dex_await (dex_timeout_new_msec (g_random_int_range (0, PROVIDER_INIT_MAX_DELAY_MSEC)), &error))
    {
      if (!g_error_matches (error, DEX_ERROR, DEX_ERROR_TIMED_OUT))
        return dex_future_new_for_error (g_steal_pointer (&error));
      g_clear_error (&error);
    }

  self->voices = g_steal_pointer (&cached_voices);

  self->proxy = dex_await_object (
      speech_provider_proxy_proxy_new_future (self->connection,
                                              self->voices
                                                ? G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START
                                                : G_DBUS_PROXY_FLAGS_NONE,
                                              self->bus_name,
                                              self->obj_path),
      &error);
  if (error)
    return dex_future_new_for_error (g_steal_pointer (&error));

  g_signal_connect_object (self->proxy, "notify::voices",
                           G_CALLBACK (on_voices_notify), self, 0);

  voices = speech_provider_proxy_get_voices (self->proxy);
  if (voices)
    {
      g_clear_pointer (&self->voices, g_variant_unref);
      self->voices = g_variant_ref (voices);
      cache_provider_voices (self->bus_name, voices);
    }

  return dex_future_new_true ();
}

static void
on_name_owner_notify (GDBusProxy *proxy,
                      GParamSpec *pspec,
                      DexPromise *promise)
{
  if (g_dbus_proxy_get_name_owner (proxy))
    {
      dex_promise_resolve_boolean (promise, TRUE);
      g_signal_handlers_disconnect_by_data (proxy, promise);
    }
}

static DexFuture *
wait_for_name_owner (XdpVoiceProvider *self)
{
  DexPromise *promise;

  if (g_dbus_proxy_get_name_owner (G_DBUS_PROXY (self->proxy)))
    return dex_future_new_for_boolean (TRUE);

  promise = dex_promise_new ();
  g_signal_connect_data (self->proxy, "notify::g-name-owner",
                         G_CALLBACK (on_name_owner_notify),
                         dex_ref (promise),
                         (GClosureNotify) dex_unref,
                         0);

  return DEX_FUTURE (promise);
}

DexFuture *
xdp_voice_provider_ensure_active (XdpVoiceProvider *self)
{
  DexFuture *start;

  if (!self->proxy)
    return dex_future_new_for_boolean (FALSE);

  if (g_dbus_proxy_get_name_owner (G_DBUS_PROXY (self->proxy)))
    return dex_future_new_for_boolean (TRUE);

  start = dex_dbus_connection_call (self->connection,
                                    "org.freedesktop.DBus",
                                    "/org/freedesktop/DBus",
                                    "org.freedesktop.DBus",
                                    "StartServiceByName",
                                    g_variant_new ("(su)", self->bus_name, 0),
                                    NULL,
                                    G_DBUS_CALL_FLAGS_NONE,
                                    -1);

  return dex_future_all (start, wait_for_name_owner (self), NULL);
}

static DexFuture *
xdp_voice_provider_init_voices (XdpVoiceProvider *self)
{
  return dex_future_first (
      dex_scheduler_spawn (NULL, 0, init_provider_fiber, self, NULL),
      dex_cancellable_new_from_cancellable (self->cancellable),
      NULL);
}

static XdpVoiceProvider *
xdp_voice_provider_new (const char      *bus_name,
                        const char      *obj_path,
                        GDBusConnection *connection)
{
  XdpVoiceProvider *self = g_object_new (XDP_TYPE_VOICE_PROVIDER, NULL);

  self->bus_name = g_strdup (bus_name);
  self->obj_path = g_strdup (obj_path);
  self->connection = connection;
  self->cancellable = g_cancellable_new ();

  return self;
}

static void
xdp_voice_provider_init (XdpVoiceProvider *self)
{
}

static void
xdp_voice_provider_dispose (GObject *object)
{
  XdpVoiceProvider *self = XDP_VOICE_PROVIDER (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->voices, g_variant_unref);
  g_clear_object (&self->proxy);
  g_clear_pointer (&self->bus_name, g_free);
  g_clear_pointer (&self->obj_path, g_free);

  G_OBJECT_CLASS (xdp_voice_provider_parent_class)->dispose (object);
}

static void
xdp_voice_provider_class_init (XdpVoiceProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_voice_provider_dispose;

  provider_signals[VOICES_CHANGED] =
      g_signal_new ("voices-changed",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    0, NULL, NULL, NULL,
                    G_TYPE_NONE, 0);
}

/* XdpVoiceProviderStore */

typedef struct _XdpVoiceProviderStore
{
  GObject parent;

  GDBusConnection *connection;      /* (unowned) */
  GAppInfoMonitor *app_info_monitor; /* (owned) */
  GHashTable      *providers;        /* (owned) char * -> XdpVoiceProvider */
  DexFuture       *batch;           /* (owned) (nullable) */
  DexFuture       *ready;           /* (owned) */
} XdpVoiceProviderStore;

G_DEFINE_FINAL_TYPE (XdpVoiceProviderStore,
                     xdp_voice_provider_store,
                     G_TYPE_OBJECT)

enum
{
  STORE_CHANGED,
  N_STORE_SIGNALS,
};

static unsigned int store_signals[N_STORE_SIGNALS] = { 0 };

static void
on_provider_voices_changed (XdpVoiceProvider      *provider,
                            XdpVoiceProviderStore *store)
{
  g_signal_emit (store, store_signals[STORE_CHANGED], 0);
}

static DexFuture *
on_batch_complete (DexFuture *completed,
                   gpointer   user_data)
{
  XdpVoiceProviderStore *store = XDP_VOICE_PROVIDER_STORE (user_data);

  g_signal_emit (store, store_signals[STORE_CHANGED], 0);

  if (DEX_IS_PROMISE (store->ready) && dex_future_is_pending (store->ready))
    dex_promise_resolve_boolean (DEX_PROMISE (store->ready), TRUE);

  return NULL;
}

static void
xdp_voice_provider_store_collect (XdpVoiceProviderStore *store)
{
  g_autolist (GDesktopAppInfo) implementations = NULL;
  g_autoptr (GHashTable) new_names = NULL;
  g_autoptr (GPtrArray) futures = NULL;
  GHashTableIter iter;
  gboolean removed = FALSE;
  char *bus_name;
  char *obj_path;

  new_names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  implementations = g_desktop_app_info_get_implementations (PROVIDER_INTERFACE);

  for (GList *l = implementations; l; l = l->next)
    {
      GDesktopAppInfo *app_info = G_DESKTOP_APP_INFO (l->data);
      const char *app_id = g_app_info_get_id (G_APP_INFO (app_info));
      g_autofree char *name = NULL;
      g_auto (GStrv) split = NULL;
      g_autofree char *partial = NULL;

      if (!app_id || !g_str_has_suffix (app_id, ".desktop"))
        continue;

      name = g_strndup (app_id, strlen (app_id) - strlen (".desktop"));

      if (g_hash_table_contains (new_names, name))
        continue;

      split = g_strsplit (name, ".", 0);
      partial = g_strjoinv ("/", split);

      g_hash_table_insert (new_names,
                           g_strdup (name),
                           g_strdup_printf ("/%s", partial));
    }

  g_hash_table_iter_init (&iter, store->providers);
  while (g_hash_table_iter_next (&iter, (gpointer *) &bus_name, NULL))
    {
      if (!g_hash_table_contains (new_names, bus_name))
        {
          g_hash_table_iter_remove (&iter);
          removed = TRUE;
        }
    }

  if (removed)
    g_signal_emit (store, store_signals[STORE_CHANGED], 0);

  futures = g_ptr_array_new_with_free_func (dex_unref);

  g_hash_table_iter_init (&iter, new_names);
  while (g_hash_table_iter_next (&iter, (gpointer *) &bus_name, (gpointer *) &obj_path))
    {
      XdpVoiceProvider *provider;

      if (g_hash_table_contains (store->providers, bus_name))
        continue;

      provider = xdp_voice_provider_new (bus_name, obj_path,
                                         store->connection);
      g_signal_connect_object (provider, "voices-changed",
                               G_CALLBACK (on_provider_voices_changed),
                               store, 0);
      g_hash_table_insert (store->providers, g_strdup (bus_name), provider);
      g_ptr_array_add (futures,
                       xdp_voice_provider_init_voices (provider));
    }

  if (futures->len > 0)
    {
      DexFuture *batch;

      batch = dex_future_finally (
          dex_future_allv ((DexFuture **) futures->pdata, futures->len),
          on_batch_complete,
          store, NULL);

      if (store->batch)
        store->batch = dex_future_all (store->batch, batch, NULL);
      else
        store->batch = batch;
    }
  else if (DEX_IS_PROMISE (store->ready) && dex_future_is_pending (store->ready))
    {
      dex_promise_resolve_boolean (DEX_PROMISE (store->ready), TRUE);
    }
}

static void
on_app_info_changed (GAppInfoMonitor       *monitor,
                     XdpVoiceProviderStore *store)
{
  xdp_voice_provider_store_collect (store);
}

static void
xdp_voice_provider_store_init (XdpVoiceProviderStore *store)
{
}

static void
xdp_voice_provider_store_dispose (GObject *object)
{
  XdpVoiceProviderStore *store = XDP_VOICE_PROVIDER_STORE (object);

  dex_clear (&store->batch);
  dex_clear (&store->ready);
  g_clear_pointer (&store->providers, g_hash_table_unref);
  g_clear_object (&store->app_info_monitor);

  G_OBJECT_CLASS (xdp_voice_provider_store_parent_class)->dispose (object);
}

static void
xdp_voice_provider_store_class_init (XdpVoiceProviderStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_voice_provider_store_dispose;

  store_signals[STORE_CHANGED] =
      g_signal_new ("changed",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    0, NULL, NULL, NULL,
                    G_TYPE_NONE, 0);
}

XdpVoiceProviderStore *
xdp_voice_provider_store_new (GDBusConnection *connection)
{
  g_autoptr (XdpVoiceProviderStore) store = NULL;

  store = g_object_new (XDP_TYPE_VOICE_PROVIDER_STORE, NULL);
  store->connection = connection;
  store->providers =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  store->ready = DEX_FUTURE (dex_promise_new ());

  store->app_info_monitor = g_app_info_monitor_get ();
  g_signal_connect_object (store->app_info_monitor, "changed",
                           G_CALLBACK (on_app_info_changed), store, 0);

  xdp_voice_provider_store_collect (store);

  return g_steal_pointer (&store);
}

DexFuture *
xdp_voice_provider_store_wait_ready (XdpVoiceProviderStore *store)
{
  return dex_ref (store->ready);
}

XdpVoiceProvider *
xdp_voice_provider_store_lookup (XdpVoiceProviderStore *store,
                                 const char            *provider_id)
{
  return g_hash_table_lookup (store->providers, provider_id);
}

GList *
xdp_voice_provider_store_get_providers (XdpVoiceProviderStore *store)
{
  return g_hash_table_get_values (store->providers);
}
