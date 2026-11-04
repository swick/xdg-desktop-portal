/*
 * Copyright © 2025 GNOME Foundation Inc.
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
 *       Eitan Isaacson <eitan@monotonous.org>
 */

#include "speech.h"

#include <gio/gio.h>
#include <glib/gi18n.h>

#include "speech-provider-dbus.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-glib-backports.h"
#include "xdp-permissions.h"
#include "xdp-request-dex.h"
#include "xdp-session-dex.h"
#include "xdp-utils.h"

static char *
desktop_id_to_bus_name (const char *desktop_id)
{
  if (g_str_has_suffix (desktop_id, ".desktop"))
    return g_strndup (desktop_id, strlen (desktop_id) - strlen (".desktop"));
  return g_strdup (desktop_id);
}

static char *
bus_name_to_object_path (const char *bus_name)
{
  g_auto (GStrv) split = g_strsplit (bus_name, ".", 0);
  g_autofree char *partial = g_strjoinv ("/", split);
  return g_strdup_printf ("/%s", partial);
}

static GVariant *
fetch_voices (GDBusConnection *connection,
              const char      *bus_name,
              const char      *obj_path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = NULL;

  result = dex_await_variant (
      dex_dbus_connection_call (connection,
                                bus_name, obj_path,
                                "org.freedesktop.DBus.Properties",
                                "Get",
                                g_variant_new ("(ss)",
                                               "org.freedesktop.Speech.Provider",
                                               "Voices"),
                                G_VARIANT_TYPE ("(v)"),
                                G_DBUS_CALL_FLAGS_NONE, 5000),
      &error);
  if (!result)
    return NULL;

  {
    g_autoptr (GVariant) inner = g_variant_get_child_value (result, 0);
    return g_variant_get_variant (inner);
  }
}

/* XdpSpeechProvider — per-provider actor that manages a single provider's
 * proxy and voice state. Runs its own fiber to handle async voice fetching
 * when the provider (re)starts. */

struct _XdpSpeechProvider
{
  GObject parent_instance;
  GDBusConnection *connection; /* (owned) */
  char *bus_name; /* (owned) */
  char *obj_path; /* (owned) */
  char *name; /* (owned) */
  GVariant *voices; /* (owned) (nullable) */
  SpeechProviderProxy *proxy; /* (owned) (nullable) */
  DexPromise *ready; /* (owned) (nullable) */
  DexPromise *name_owner_appeared; /* (owned) (nullable) */
  unsigned int props_changed_sub_id;
};

#define XDP_TYPE_SPEECH_PROVIDER (xdp_speech_provider_get_type ())
G_DECLARE_FINAL_TYPE (XdpSpeechProvider,
                      xdp_speech_provider,
                      XDP, SPEECH_PROVIDER,
                      GObject)

enum
{
  PROVIDER_CHANGED,
  N_PROVIDER_SIGNALS,
};

static guint provider_signals[N_PROVIDER_SIGNALS];

G_DEFINE_FINAL_TYPE (XdpSpeechProvider, xdp_speech_provider, G_TYPE_OBJECT)

static const char *
xdp_speech_provider_get_name (XdpSpeechProvider *self)
{
  return self->name;
}

static GVariant *
xdp_speech_provider_get_voices (XdpSpeechProvider *self)
{
  return self->voices;
}

static SpeechProviderProxy *
xdp_speech_provider_get_proxy (XdpSpeechProvider *self)
{
  return self->proxy;
}

static DexFuture *
xdp_speech_provider_await_ready (XdpSpeechProvider *self)
{
  if (!self->ready)
    return dex_future_new_true ();
  return dex_ref (DEX_FUTURE (self->ready));
}

static void
on_properties_changed (GDBusConnection *connection,
                       const char      *sender,
                       const char      *object_path,
                       const char      *interface_name,
                       const char      *signal_name,
                       GVariant        *parameters,
                       gpointer         user_data)
{
  XdpSpeechProvider *self = user_data;
  const char *iface;
  g_autoptr (GVariant) changed_props = NULL;
  g_autoptr (GVariant) voices = NULL;

  g_variant_get (parameters, "(&s@a{sv}^a&s)", &iface, &changed_props, NULL);

  if (g_strcmp0 (iface, "org.freedesktop.Speech.Provider") != 0)
    return;

  voices = g_variant_lookup_value (changed_props, "Voices", NULL);
  if (!voices)
    return;

  g_clear_pointer (&self->voices, g_variant_unref);
  self->voices = g_steal_pointer (&voices);
  g_signal_emit (self, provider_signals[PROVIDER_CHANGED], 0);
}

static void
on_name_owner_changed (GDBusProxy        *proxy,
                       GParamSpec        *pspec,
                       XdpSpeechProvider *self)
{
  g_autofree char *owner = g_dbus_proxy_get_name_owner (proxy);

  if (!owner)
    return;
  if (!self->name_owner_appeared)
    return;

  dex_promise_resolve_boolean (self->name_owner_appeared, TRUE);
  dex_clear (&self->name_owner_appeared);
}

static DexFuture *
xdp_speech_provider_run (XdpSpeechProvider *self)
{
  g_autoptr (GError) error = NULL;
  GDBusProxyFlags flags;

  flags = G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES;
  if (self->voices)
    flags |= G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START;

  self->proxy = dex_await_object (
      speech_provider_proxy_proxy_new_future (self->connection, flags,
                                               self->bus_name, self->obj_path),
      &error);
  if (error)
    {
      if (self->ready)
        {
          dex_promise_resolve_boolean (self->ready, TRUE);
          dex_clear (&self->ready);
        }
      return dex_future_new_for_error (g_steal_pointer (&error));
    }

  self->props_changed_sub_id =
      g_dbus_connection_signal_subscribe (self->connection,
                                          self->bus_name,
                                          "org.freedesktop.DBus.Properties",
                                          "PropertiesChanged",
                                          self->obj_path,
                                          NULL,
                                          G_DBUS_SIGNAL_FLAGS_NONE,
                                          on_properties_changed,
                                          self, NULL);

  g_signal_connect (self->proxy, "notify::g-name-owner",
                    G_CALLBACK (on_name_owner_changed), self);

  if (!self->voices)
    self->voices = fetch_voices (self->connection, self->bus_name,
                                 self->obj_path);

  if (self->ready)
    {
      dex_promise_resolve_boolean (self->ready, TRUE);
      dex_clear (&self->ready);
    }

  g_signal_emit (self, provider_signals[PROVIDER_CHANGED], 0);

  for (;;)
    {
      GVariant *new_voices = NULL;

      self->name_owner_appeared = dex_promise_new ();
      dex_await_boolean (DEX_FUTURE (dex_ref (self->name_owner_appeared)), &error);
      if (error)
        break;

      new_voices = fetch_voices (self->connection, self->bus_name,
                                  self->obj_path);
      if (!new_voices)
        continue;

      g_clear_pointer (&self->voices, g_variant_unref);
      self->voices = new_voices;
      g_signal_emit (self, provider_signals[PROVIDER_CHANGED], 0);
    }

  return dex_future_new_true ();
}

static XdpSpeechProvider *
xdp_speech_provider_new (GDBusConnection *connection,
                          const char      *bus_name,
                          const char      *name,
                          GVariant        *cached_voices)
{
  XdpSpeechProvider *self;

  self = g_object_new (XDP_TYPE_SPEECH_PROVIDER, NULL);
  self->connection = g_object_ref (connection);
  self->bus_name = g_strdup (bus_name);
  self->obj_path = bus_name_to_object_path (bus_name);
  self->name = g_strdup (name);
  self->ready = dex_promise_new ();

  if (cached_voices)
    self->voices = g_variant_ref (cached_voices);

  dex_future_disown (dex_scheduler_spawn (NULL, 0,
                                          (DexFiberFunc) xdp_speech_provider_run,
                                          g_object_ref (self), g_object_unref));

  return self;
}

static void
xdp_speech_provider_dispose (GObject *object)
{
  XdpSpeechProvider *self = XDP_SPEECH_PROVIDER (object);

  if (self->props_changed_sub_id && self->connection)
    {
      g_dbus_connection_signal_unsubscribe (self->connection,
                                            self->props_changed_sub_id);
      self->props_changed_sub_id = 0;
    }

  if (self->name_owner_appeared)
    {
      dex_promise_reject (self->name_owner_appeared,
                          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                              "Provider disposed"));
      dex_clear (&self->name_owner_appeared);
    }

  dex_clear (&self->ready);
  g_clear_object (&self->proxy);
  g_clear_pointer (&self->voices, g_variant_unref);
  g_clear_pointer (&self->bus_name, g_free);
  g_clear_pointer (&self->obj_path, g_free);
  g_clear_pointer (&self->name, g_free);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (xdp_speech_provider_parent_class)->dispose (object);
}

static void
xdp_speech_provider_init (XdpSpeechProvider *self)
{
}

static void
xdp_speech_provider_class_init (XdpSpeechProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_speech_provider_dispose;

  provider_signals[PROVIDER_CHANGED] =
      g_signal_new ("changed",
                    G_TYPE_FROM_CLASS (object_class),
                    G_SIGNAL_RUN_LAST,
                    0, NULL, NULL, NULL,
                    G_TYPE_NONE, 0);
}

/* XdpSpeechProviders — collection actor that discovers providers via intents,
 * creates per-provider actors, and caches voice data. */

struct _XdpSpeechProviders
{
  GObject parent_instance;
  GDBusConnection *connection; /* (owned) */
  GHashTable *providers; /* (owned) bus_name → XdpSpeechProvider */
  DexChannel *channel; /* (owned) */
  GAppInfoMonitor *monitor; /* (owned) */
  gulong monitor_changed_id;
  DexPromise *ready; /* (owned) (nullable) */
};

#define XDP_TYPE_SPEECH_PROVIDERS (xdp_speech_providers_get_type ())
G_DECLARE_FINAL_TYPE (XdpSpeechProviders,
                      xdp_speech_providers,
                      XDP, SPEECH_PROVIDERS,
                      GObject)

enum
{
  PROVIDERS_CHANGED,
  N_PROVIDERS_SIGNALS,
};

static guint providers_signals[N_PROVIDERS_SIGNALS];

G_DEFINE_FINAL_TYPE (XdpSpeechProviders, xdp_speech_providers, G_TYPE_OBJECT)

static void
save_voice_cache (XdpSpeechProviders *self)
{
  GHashTableIter iter;
  const char *bus_name;
  XdpSpeechProvider *provider;
  g_auto (GVariantBuilder) builder =
      G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a{sv}"));

  g_hash_table_iter_init (&iter, self->providers);
  while (g_hash_table_iter_next (&iter, (gpointer *) &bus_name,
                                 (gpointer *) &provider))
    {
      GVariant *voices = xdp_speech_provider_get_voices (provider);
      if (voices)
        g_variant_builder_add (&builder, "{sv}", bus_name, voices);
    }

  dex_future_disown (
      xdp_dbus_impl_permission_store_call_set_value_future (
          xdp_get_permission_store (),
          SPEECH_PERMISSION_TABLE,
          TRUE,
          SPEECH_PERMISSION_ID,
          g_variant_new_variant (g_variant_builder_end (&builder))));
}

static GVariant *
load_voice_cache (void)
{
  g_autoptr (XdpDbusImplPermissionStoreLookupResult) result =
      dex_await_boxed (xdp_dbus_impl_permission_store_call_lookup_future (
                           xdp_get_permission_store (),
                           SPEECH_PERMISSION_TABLE,
                           SPEECH_PERMISSION_ID),
                       NULL);

  if (!result)
    return NULL;

  {
    g_autoptr (GVariant) data = g_variant_get_variant (result->data);
    if (data && g_variant_is_of_type (data, G_VARIANT_TYPE ("a{sv}")))
      return g_steal_pointer (&data);
  }

  return NULL;
}

static void
on_provider_changed (XdpSpeechProvider  *provider,
                     XdpSpeechProviders *self)
{
  save_voice_cache (self);
  g_signal_emit (self, providers_signals[PROVIDERS_CHANGED], 0);
}

static void
on_app_info_changed (GAppInfoMonitor    *monitor,
                     XdpSpeechProviders *self)
{
  dex_future_disown (dex_channel_send (self->channel,
                                       dex_future_new_true ()));
}

static gboolean
update_providers (XdpSpeechProviders *self,
                  GVariant           *voice_cache)
{
  size_t n_infos = 0;
  GAppInfo **infos = NULL;
  g_autoptr (GHashTable) discovered =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  gboolean changed = FALSE;

  infos = g_desktop_app_info_get_for_intent ("org.freedesktop.Speech.Provider",
                                              NULL, &n_infos);

  for (size_t i = 0; i < n_infos; i++)
    {
      g_autofree char *bus_name =
          desktop_id_to_bus_name (g_app_info_get_id (infos[i]));
      g_autoptr (GVariant) cached_voices = NULL;
      XdpSpeechProvider *provider;

      g_hash_table_add (discovered, g_strdup (bus_name));

      if (g_hash_table_contains (self->providers, bus_name))
        continue;

      if (voice_cache)
        cached_voices = g_variant_lookup_value (voice_cache, bus_name, NULL);

      provider = xdp_speech_provider_new (self->connection, bus_name,
                                           g_app_info_get_name (infos[i]),
                                           cached_voices);

      g_signal_connect_object (provider, "changed",
                               G_CALLBACK (on_provider_changed), self,
                               G_CONNECT_DEFAULT);

      g_hash_table_insert (self->providers, g_strdup (bus_name), provider);
      changed = TRUE;
    }

  for (size_t i = 0; i < n_infos; i++)
    g_object_unref (infos[i]);
  g_free (infos);

  {
    GHashTableIter iter;
    const char *bus_name;

    g_hash_table_iter_init (&iter, self->providers);
    while (g_hash_table_iter_next (&iter, (gpointer *) &bus_name, NULL))
      {
        if (g_hash_table_contains (discovered, bus_name))
          continue;

        g_hash_table_iter_remove (&iter);
        changed = TRUE;
      }
  }

  return changed;
}

static DexFuture *
xdp_speech_providers_run (XdpSpeechProviders *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) voice_cache = NULL;

  voice_cache = load_voice_cache ();

  for (;;)
    {
      if (update_providers (self, voice_cache))
        {
          save_voice_cache (self);
          g_signal_emit (self, providers_signals[PROVIDERS_CHANGED], 0);
        }

      g_clear_pointer (&voice_cache, g_variant_unref);

      if (self->ready)
        {
          GHashTableIter iter;
          XdpSpeechProvider *provider;

          g_hash_table_iter_init (&iter, self->providers);
          while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &provider))
            dex_await (xdp_speech_provider_await_ready (provider), NULL);

          dex_promise_resolve_boolean (self->ready, TRUE);
          dex_clear (&self->ready);
        }

      dex_await (dex_channel_receive (self->channel), &error);
      if (error)
        break;
    }

  return dex_future_new_true ();
}

static DexFuture *
xdp_speech_providers_await_ready (XdpSpeechProviders *self)
{
  if (!self->ready)
    return dex_future_new_true ();
  return dex_ref (DEX_FUTURE (self->ready));
}

static GHashTable *
xdp_speech_providers_get_providers (XdpSpeechProviders *self)
{
  return self->providers;
}

static void
xdp_speech_providers_dispose (GObject *object)
{
  XdpSpeechProviders *self = XDP_SPEECH_PROVIDERS (object);

  g_clear_signal_handler (&self->monitor_changed_id, self->monitor);

  if (self->channel)
    {
      dex_channel_close_send (self->channel);
      dex_channel_close_receive (self->channel);
      dex_clear (&self->channel);
    }

  g_clear_pointer (&self->providers, g_hash_table_unref);
  dex_clear (&self->ready);
  g_clear_object (&self->monitor);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (xdp_speech_providers_parent_class)->dispose (object);
}

static void
xdp_speech_providers_init (XdpSpeechProviders *self)
{
}

static void
xdp_speech_providers_class_init (XdpSpeechProvidersClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_speech_providers_dispose;

  providers_signals[PROVIDERS_CHANGED] =
      g_signal_new ("changed",
                    G_TYPE_FROM_CLASS (object_class),
                    G_SIGNAL_RUN_LAST,
                    0, NULL, NULL, NULL,
                    G_TYPE_NONE, 0);
}

static XdpSpeechProviders *
xdp_speech_providers_new (GDBusConnection *connection)
{
  XdpSpeechProviders *self;

  self = g_object_new (XDP_TYPE_SPEECH_PROVIDERS, NULL);
  self->connection = g_object_ref (connection);
  self->channel = dex_channel_new (4);
  self->providers =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  self->monitor = g_app_info_monitor_get ();
  self->monitor_changed_id =
      g_signal_connect (self->monitor, "changed",
                        G_CALLBACK (on_app_info_changed), self);

  self->ready = dex_promise_new ();

  dex_future_disown (dex_scheduler_spawn (NULL, 0,
                                          (DexFiberFunc) xdp_speech_providers_run,
                                          g_object_ref (self), g_object_unref));

  return self;
}

/* ---------------------------------------------------------------------------------------------------- *
 * XdpSpeech — portal implementation
 * ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  XdpSessionDex *session;
} XdpSpeechSession;

static void
xdp_speech_session_free (XdpSpeechSession *speech_session)
{
  g_clear_object (&speech_session->session);
  g_free (speech_session);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpSpeechSession, xdp_speech_session_free)

struct _XdpSpeech
{
  XdpDbusSpeechSkeleton parent_instance;
  XdpContext *context;
  XdpDbusImplAccess *access_impl;
  XdpSessionDexStore *sessions;
  XdpSpeechProviders *providers;
  gulong permission_changed_handler_id;
};

#define XDP_TYPE_SPEECH (xdp_speech_get_type ())
G_DECLARE_FINAL_TYPE (XdpSpeech, xdp_speech, XDP, SPEECH, XdpDbusSpeechSkeleton);

static void xdp_speech_iface_init (XdpDbusSpeechIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpSpeech,
                               xdp_speech,
                               XDP_DBUS_TYPE_SPEECH_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SPEECH, xdp_speech_iface_init));

static DexFuture *
ask_speech_permissions_future (XdpSpeech *speech, XdpRequestDex *request, const char *parent_window)
{
  XdpAppInfo *app_info = xdp_request_dex_get_app_info (request);
  const char *app_name = xdp_app_info_get_app_display_name (app_info);
  g_autoptr (XdpDbusImplAccessAccessDialogResult) result = NULL;
  g_auto (GVariantBuilder) access_opt_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr (GError) error = NULL;
  g_autofree gchar *title = NULL;
  g_autofree gchar *subtitle = NULL;
  const gchar *body;
  guint perms = dex_await_uint (
      xdp_permission_get_future (app_info, SPEECH_PERMISSION_TABLE, SPEECH_PERMISSION_ID), &error);

  if (error)
    return dex_future_new_for_error (g_steal_pointer (&error));

  if (perms == XDP_PERMISSION_YES)
    return dex_future_new_true ();

  if (perms == XDP_PERMISSION_NO)
    {
      return dex_future_new_for_error (
          g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "No permission for speech"));
    }

  g_variant_builder_add (&access_opt_builder, "{sv}", "deny_label",
                         g_variant_new_string (_ ("Deny")));
  g_variant_builder_add (&access_opt_builder, "{sv}", "grant_label",
                         g_variant_new_string (_ ("Allow")));
  g_variant_builder_add (&access_opt_builder, "{sv}", "icon",
                         g_variant_new_string ("preferences-desktop-wallpaper-symbolic"));

  if (app_name)
    {
      title = g_strdup_printf (_ ("Allow %s to Use Speech Synthesis?"), app_name);
      subtitle =
          g_strdup_printf (_ ("%s wants to use external speech synthesis services"), app_name);
    }
  else
    {
      title = g_strdup (_ ("Allow Apps to Use Speech Synthesis?"));
      subtitle = g_strdup (_ ("An app wants to use external speech synthesis services"));
    }

  body = _ ("This permission can be changed at any time from the privacy settings");

  result = dex_await_boxed (xdp_dbus_impl_access_call_access_dialog_future (
                                speech->access_impl, xdp_request_dex_get_object_path (request),
                                xdp_app_info_get_id (app_info), parent_window, title, subtitle,
                                body, g_variant_builder_end (&access_opt_builder)),
                            &error);

  if (error)
    return dex_future_new_for_error (g_steal_pointer (&error));

  perms = result->response == 0 ? XDP_PERMISSION_YES : XDP_PERMISSION_NO;

  if (!dex_await_boolean (
          xdp_permission_set_future (app_info, SPEECH_PERMISSION_TABLE, SPEECH_PERMISSION_ID, perms), &error))
    g_warning ("Setting unset permission failed: %s", error->message);

  if (perms != XDP_PERMISSION_YES)
    {
      return dex_future_new_for_error (
          g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "No permission for speech"));
    }

  return dex_future_new_true ();
}

static void
emit_voices_changed_for_session (XdpSpeechSession *speech_session, gpointer user_data)
{
  GDBusConnection *connection = xdp_session_dex_get_connection (speech_session->session);
  XdpAppInfo *app_info = xdp_session_dex_get_app_info (speech_session->session);
  const char *sender = xdp_app_info_get_sender (app_info);

  if (!connection)
    return;

  g_autoptr (GError) error = NULL;
  if (!g_dbus_connection_emit_signal (
          connection, sender, "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Speech",
          "VoicesChanged",
          g_variant_new ("(o)", xdp_session_dex_get_object_path (speech_session->session)),
          &error))
    {
      g_warning ("Failed to emit VoicesChanged signal: %s", error->message);
    }
}

static void
on_speech_permission_store_changed (XdpSpeech                     *speech,
                                    const char                    *arg_table,
                                    const char                    *arg_id,
                                    gboolean                       arg_deleted,
                                    GVariant                      *arg_data,
                                    GVariant                      *arg_permissions,
                                    XdpDbusImplPermissionStore    *permission_store)
{
  if (g_strcmp0 (arg_table, SPEECH_PERMISSION_TABLE) != 0 ||
      g_strcmp0 (arg_id, SPEECH_PERMISSION_ID) != 0)
    return;

  xdp_session_dex_store_foreach (speech->sessions,
                                 (GFunc) emit_voices_changed_for_session, NULL);
}

static void
on_providers_changed (XdpSpeechProviders *providers,
                      XdpSpeech          *speech)
{
  xdp_session_dex_store_foreach (speech->sessions,
                                 (GFunc) emit_voices_changed_for_session, NULL);
}

static gboolean
handle_create_session (XdpDbusSpeech         *object,
                       GDBusMethodInvocation *invocation,
                       GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  g_autoptr (GError) error = NULL;
  g_autoptr (XdpSessionDex) session = NULL;
  g_autoptr (XdpSpeechSession) speech_session = NULL;

  dex_await (xdp_speech_providers_await_ready (speech->providers), NULL);

  session = dex_await_object (
      xdp_session_dex_new (speech->context, xdp_invocation_get_app_info (invocation),
                           G_DBUS_INTERFACE_SKELETON (object), NULL, arg_options),
      &error);

  if (!session)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  speech_session = g_new0 (XdpSpeechSession, 1);
  speech_session->session = g_object_ref (session);

  xdp_dbus_speech_complete_create_session (object, g_steal_pointer (&invocation),
                                           xdp_session_dex_get_object_path (session));

  xdp_session_dex_store_take_session (speech->sessions, g_steal_pointer (&speech_session));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_get_provider_details (XdpDbusSpeech         *object,
                             GDBusMethodInvocation *invocation,
                             const char            *arg_session_handle,
                             const char            *arg_parent_window,
                             const char            *arg_provider_id,
                             GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  XdpRequestDex *request = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
  g_auto (GVariantBuilder) request_data_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr (GError) error = NULL;

  if (!xdp_session_dex_store_lookup_session (speech->sessions, arg_session_handle, app_info))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation), G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED, "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request =
      dex_await_object (xdp_request_dex_new (speech->context, app_info,
                                             G_DBUS_INTERFACE_SKELETON (object), NULL, arg_options),
                        &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_request_dex_export (request, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_speech_complete_get_provider_details (object, g_steal_pointer (&invocation),
                                                 xdp_request_dex_get_object_path (request));

  if (dex_await_boolean (ask_speech_permissions_future (speech, request, arg_parent_window), NULL))
    {
      XdpSpeechProvider *provider =
          g_hash_table_lookup (xdp_speech_providers_get_providers (speech->providers), arg_provider_id);
      if (provider != NULL)
        {
          g_variant_builder_add (&request_data_builder, "{sv}", "name",
                                 g_variant_new_string (xdp_speech_provider_get_name (provider)));
          response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
        }
      else
        {
          response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
        }
    }

  xdp_request_dex_emit_response (request, response, g_variant_builder_end (&request_data_builder));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static GVariant *
get_voices (XdpSpeech *speech)
{
  GHashTableIter providers_iter;
  XdpSpeechProvider *provider = NULL;
  char *provider_id = NULL;
  g_auto (GVariantBuilder) voices_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("aa{sv}"));

  g_hash_table_iter_init (&providers_iter, xdp_speech_providers_get_providers (speech->providers));
  while (g_hash_table_iter_next (&providers_iter, (gpointer *) &provider_id,
                                 (gpointer *) &provider))
    {
      GVariant *voices = xdp_speech_provider_get_voices (provider);
      GVariant *voice_name = NULL;
      GVariant *voice_id = NULL;
      GVariant *voice_format = NULL;
      GVariant *voice_features = NULL;
      GVariant *voice_languages = NULL;
      g_autoptr (GVariantIter) iter = NULL;

      if (voices == NULL)
        continue;

      iter = g_variant_iter_new (voices);

      while (g_variant_iter_loop (iter, "(@s@s@s@t@as)", &voice_name, &voice_id, &voice_format,
                                  &voice_features, &voice_languages))
        {
          GVariantDict dict;
          g_variant_dict_init (&dict, NULL);

          g_variant_dict_insert_value (&dict, "name", voice_name);
          g_variant_dict_insert_value (&dict, "identifier", voice_id);
          g_variant_dict_insert_value (&dict, "output-format", voice_format);
          g_variant_dict_insert_value (&dict, "features", voice_features);
          g_variant_dict_insert_value (&dict, "languages", voice_languages);
          g_variant_dict_insert (&dict, "provider", "s", provider_id);

          g_variant_builder_add_value (&voices_builder, g_variant_dict_end (&dict));
        }
    }

  return g_variant_builder_end (&voices_builder);
}

static gboolean
handle_get_voices (XdpDbusSpeech         *object,
                   GDBusMethodInvocation *invocation,
                   const char            *arg_session_handle,
                   const char            *arg_parent_window,
                   GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  XdpRequestDex *request = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr (GError) error = NULL;

  if (!xdp_session_dex_store_lookup_session (speech->sessions, arg_session_handle, app_info))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation), G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED, "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request =
      dex_await_object (xdp_request_dex_new (speech->context, app_info,
                                             G_DBUS_INTERFACE_SKELETON (object), NULL, arg_options),
                        &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_request_dex_export (request, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_speech_complete_get_voices (object, g_steal_pointer (&invocation),
                                       xdp_request_dex_get_object_path (request));

  {
    guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
    g_auto (GVariantBuilder) request_data_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (dex_await_boolean (ask_speech_permissions_future (speech, request, arg_parent_window),
                           NULL))
      {
        g_variant_builder_add (&request_data_builder, "{sv}", "voices",
                               get_voices (speech));

        response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
      }

    xdp_request_dex_emit_response (request, response,
                                   g_variant_builder_end (&request_data_builder));
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_synthesize (XdpDbusSpeech         *object,
                   GDBusMethodInvocation *invocation,
                   GUnixFDList           *fd_list,
                   const char            *arg_session_handle,
                   const char            *arg_parent_window,
                   const char            *arg_provider_id,
                   GVariant              *arg_pipe_fd,
                   const char            *arg_text,
                   const char            *arg_voice_id,
                   gdouble                arg_pitch,
                   gdouble                arg_rate,
                   gboolean               arg_is_ssml,
                   const char            *arg_language,
                   GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  XdpRequestDex *request = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSpeechProvider *provider = NULL;
  g_autoptr (GError) error = NULL;

  if (!xdp_session_dex_store_lookup_session (speech->sessions, arg_session_handle, app_info))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation), G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED, "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request =
      dex_await_object (xdp_request_dex_new (speech->context, app_info,
                                             G_DBUS_INTERFACE_SKELETON (object), NULL, arg_options),
                        &error);
  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!xdp_request_dex_export (request, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  provider = g_hash_table_lookup (xdp_speech_providers_get_providers (speech->providers), arg_provider_id);

  xdp_dbus_speech_complete_synthesize (object, g_steal_pointer (&invocation), fd_list,
                                       xdp_request_dex_get_object_path (request));

  {
    guint32 response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
    g_auto (GVariantBuilder) request_data_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    if (!dex_await_boolean (ask_speech_permissions_future (speech, request, arg_parent_window),
                            NULL))
      {
        response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
      }
    else if (!provider || !xdp_speech_provider_get_proxy (provider))
      {
        g_warning ("No provider found");
        response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      }
    else if (!dex_await (speech_provider_proxy_call_synthesize_future (
                             xdp_speech_provider_get_proxy (provider), arg_pipe_fd, arg_text, arg_voice_id, arg_pitch,
                             arg_rate, arg_is_ssml, arg_language, G_DBUS_CALL_FLAGS_NONE, -1,
                             fd_list),
                         &error))
      {
        response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
        g_variant_builder_add (&request_data_builder, "{sv}", "error-message",
                               g_variant_new_string (error->message));
      }

    xdp_request_dex_emit_response (request, response,
                                   g_variant_builder_end (&request_data_builder));
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_speech_iface_init (XdpDbusSpeechIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_get_provider_details = handle_get_provider_details;
  iface->handle_get_voices = handle_get_voices;
  iface->handle_synthesize = handle_synthesize;
}

static void
xdp_speech_init (XdpSpeech *speech)
{
}

static void
xdp_speech_dispose (GObject *object)
{
  XdpSpeech *speech = XDP_SPEECH (object);

  if (speech->permission_changed_handler_id)
    g_signal_handler_disconnect (xdp_get_permission_store (),
                                 speech->permission_changed_handler_id);

  g_clear_object (&speech->providers);
  g_clear_object (&speech->access_impl);
  g_clear_object (&speech->sessions);

  G_OBJECT_CLASS (xdp_speech_parent_class)->dispose (object);
}

static void
xdp_speech_class_init (XdpSpeechClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = xdp_speech_dispose;
}

static XdpSpeech *
xdp_speech_new (XdpContext *context, XdpDbusImplAccess *access_impl)
{
  XdpSpeech *speech;
  GDBusConnection *connection = xdp_context_get_connection (context);

  speech = g_object_new (XDP_TYPE_SPEECH, NULL);
  speech->context = context;
  speech->access_impl = g_object_ref (access_impl);
  speech->sessions = xdp_session_dex_store_new_wrapped (XdpSpeechSession, session,
                                                        (GDestroyNotify) xdp_speech_session_free);

  speech->providers = xdp_speech_providers_new (connection);
  g_signal_connect_object (speech->providers, "changed",
                           G_CALLBACK (on_providers_changed), speech,
                           G_CONNECT_DEFAULT);

  speech->permission_changed_handler_id =
      g_signal_connect_swapped (xdp_get_permission_store (), "changed",
                                G_CALLBACK (on_speech_permission_store_changed), speech);

  xdp_dbus_speech_set_version (XDP_DBUS_SPEECH (speech), 1);

  return speech;
}

DexFuture *
init_speech (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  g_autoptr (XdpSpeech) speech = NULL;
  XdpDbusImplAccess *access_impl;

  access_impl = xdp_context_get_access_impl (context);
  if (access_impl == NULL)
    {
      g_warning ("The speech portal requires an access impl");
      return dex_future_new_false ();
    }

  speech = xdp_speech_new (context, access_impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&speech)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);
  return dex_future_new_true ();
}
