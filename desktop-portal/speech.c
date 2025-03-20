/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "speech.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <libdex.h>

#include "speech-providers.h"
#include "xdp-app-info.h"
#include "xdp-context.h"
#include "xdp-dbus-experimental.h"
#include "xdp-permissions.h"
#include "xdp-request-dex.h"
#include "xdp-session-dex.h"
#include "xdp-utils.h"

typedef struct _XdpSpeech XdpSpeech;

struct _XdpSpeech
{
  XdpDbusSpeechSkeleton parent_instance;

  XdpContext            *context;        /* (unowned) */
  XdpDbusImplAccess     *access_impl;    /* (owned) (nullable) */
  XdpSessionDexStore    *sessions;       /* (owned) */
  XdpVoiceProviderStore *provider_store; /* (owned) */
  GVariant              *voices;         /* (owned) aa{sv} */
};

static void xdp_speech_iface_init (XdpDbusSpeechIface *iface);

#define XDP_TYPE_SPEECH (xdp_speech_get_type ())
G_DECLARE_FINAL_TYPE (XdpSpeech,
                      xdp_speech,
                      XDP, SPEECH,
                      XdpDbusSpeechSkeleton);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpSpeech,
                               xdp_speech,
                               XDP_DBUS_TYPE_SPEECH_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SPEECH, xdp_speech_iface_init));

struct _XdpSpeechSession
{
  GObject parent;

  XdpSpeech     *speech;  /* (unowned) */
  XdpSessionDex *session; /* (owned) */
};

#define XDP_TYPE_SPEECH_SESSION (xdp_speech_session_get_type ())
G_DECLARE_FINAL_TYPE (XdpSpeechSession,
                      xdp_speech_session,
                      XDP, SPEECH_SESSION,
                      GObject)

G_DEFINE_FINAL_TYPE (XdpSpeechSession,
                     xdp_speech_session,
                     G_TYPE_OBJECT)

static void
emit_voices_changed_for_session (gpointer session_wrapper,
                                 gpointer user_data)
{
  XdpSpeechSession *session = XDP_SPEECH_SESSION (session_wrapper);
  GDBusConnection *connection = xdp_session_dex_get_connection (session->session);
  XdpAppInfo *app_info = xdp_session_dex_get_app_info (session->session);
  const char *sender = xdp_app_info_get_sender (app_info);
  g_autoptr (GError) error = NULL;

  if (!connection)
    return;

  if (!g_dbus_connection_emit_signal (
          connection, sender,
          "/org/freedesktop/portal/desktop",
          "org.freedesktop.portal.experimental.Speech",
          "VoicesChanged",
          g_variant_new ("(o)", xdp_session_dex_get_object_path (session->session)),
          &error))
    g_warning ("Failed to emit VoicesChanged signal: %s", error->message);
}

static void
emit_voices_changed (XdpSpeech *speech)
{
  xdp_session_dex_store_foreach (speech->sessions,
                                 emit_voices_changed_for_session,
                                 NULL);
}

static void
xdp_speech_session_init (XdpSpeechSession *session)
{
}

static void
xdp_speech_session_finalize (GObject *object)
{
  XdpSpeechSession *session = XDP_SPEECH_SESSION (object);

  g_clear_object (&session->session);

  G_OBJECT_CLASS (xdp_speech_session_parent_class)->finalize (object);
}

static void
xdp_speech_session_class_init (XdpSpeechSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xdp_speech_session_finalize;
}

static DexFuture *
xdp_speech_session_new (XdpSpeech     *speech,
                        XdpSessionDex *speech_session)
{
  g_autoptr (XdpSpeechSession) session = NULL;

  session = g_object_new (XDP_TYPE_SPEECH_SESSION, NULL);
  session->speech = speech;
  session->session = g_object_ref (speech_session);

  return dex_future_new_take_object (g_steal_pointer (&session));
}

static void
rebuild_voices (XdpSpeech *speech)
{
  g_autoptr (GList) providers = NULL;
  g_auto (GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("aa{sv}"));

  providers = xdp_voice_provider_store_get_providers (speech->provider_store);

  for (GList *l = providers; l; l = l->next)
    {
      XdpVoiceProvider *provider = XDP_VOICE_PROVIDER (l->data);
      const char *provider_id = xdp_voice_provider_get_bus_name (provider);
      GVariant *voices = xdp_voice_provider_get_voices (provider);
      g_autoptr (GVariantIter) voice_iter = NULL;
      GVariant *voice_name = NULL;
      GVariant *voice_id = NULL;
      GVariant *voice_format = NULL;
      GVariant *voice_features = NULL;
      GVariant *voice_languages = NULL;

      if (!voices)
        continue;

      voice_iter = g_variant_iter_new (voices);
      while (g_variant_iter_loop (voice_iter, "(@s@s@s@t@as)",
                                  &voice_name, &voice_id, &voice_format,
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
          g_variant_builder_add_value (&builder, g_variant_dict_end (&dict));
        }
    }

  g_clear_pointer (&speech->voices, g_variant_unref);
  speech->voices = g_variant_ref_sink (g_variant_builder_end (&builder));
}

static void
on_provider_store_changed (XdpVoiceProviderStore *store,
                           XdpSpeech             *speech)
{
  rebuild_voices (speech);
  emit_voices_changed (speech);
}

static DexFuture *
check_speech_permission (XdpSpeech     *speech,
                         XdpAppInfo    *app_info,
                         XdpRequestDex *request,
                         const char    *parent_window)
{
  g_autoptr (XdpDbusImplAccessAccessDialogResult) result = NULL;
  g_auto (GVariantBuilder) opt_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr (GError) error = NULL;
  g_autofree char *title = NULL;
  g_autofree char *subtitle = NULL;
  const char *app_name = xdp_app_info_get_app_display_name (app_info);
  const char *body;
  XdpPermission permission;

  permission = dex_await_uint (
      xdp_permission_get_future (app_info, SPEECH_PERMISSION_TABLE, SPEECH_PERMISSION_ID), &error);
  if (error)
    return dex_future_new_for_error (g_steal_pointer (&error));

  if (permission == XDP_PERMISSION_YES)
    return dex_future_new_true ();

  if (permission == XDP_PERMISSION_NO)
    return dex_future_new_for_error (
        g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "No permission for speech"));

  if (!speech->access_impl)
    return dex_future_new_for_error (
        g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "No access dialog available"));

  g_variant_builder_add (&opt_builder, "{sv}", "deny_label",
                         g_variant_new_string (_("Deny")));
  g_variant_builder_add (&opt_builder, "{sv}", "grant_label",
                         g_variant_new_string (_("Allow")));
  g_variant_builder_add (&opt_builder, "{sv}", "icon",
                         g_variant_new_string ("audio-speakers-symbolic"));

  if (app_name)
    {
      title = g_strdup_printf (_("Allow %s to Use Speech Synthesis?"), app_name);
      subtitle = g_strdup_printf (_("%s wants to use external speech synthesis services"),
                                  app_name);
    }
  else
    {
      title = g_strdup (_("Allow Apps to Use Speech Synthesis?"));
      subtitle = g_strdup (_("An app wants to use external speech synthesis services"));
    }

  body = _("This permission can be changed at any time from the privacy settings");

  result = dex_await_boxed (
      xdp_dbus_impl_access_call_access_dialog_future (
          speech->access_impl,
          xdp_request_dex_get_object_path (request),
          xdp_app_info_get_id (app_info),
          parent_window,
          title, subtitle, body,
          g_variant_builder_end (&opt_builder)),
      &error);
  if (error)
    return dex_future_new_for_error (g_steal_pointer (&error));

  permission = result->response == 0 ? XDP_PERMISSION_YES : XDP_PERMISSION_NO;

  if (!dex_await_boolean (
          xdp_permission_set_future (app_info, SPEECH_PERMISSION_TABLE, SPEECH_PERMISSION_ID, permission),
          &error))
    g_warning ("Failed to store speech permission: %s", error->message);

  if (permission != XDP_PERMISSION_YES)
    return dex_future_new_for_error (
        g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "No permission for speech"));

  return dex_future_new_true ();
}

static gboolean
handle_create_session (XdpDbusSpeech         *object,
                       GDBusMethodInvocation *invocation,
                       GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  g_autoptr (GError) error = NULL;
  g_autoptr (XdpSessionDex) speech_session = NULL;
  g_autoptr (XdpSpeechSession) session = NULL;

  speech_session = dex_await_object (
      xdp_session_dex_new (speech->context, xdp_invocation_get_app_info (invocation),
                           G_DBUS_INTERFACE_SKELETON (object), NULL, arg_options),
      &error);

  if (!speech_session)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = dex_await_object (xdp_speech_session_new (speech, speech_session), &error);

  if (!session)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_speech_complete_create_session (object, g_steal_pointer (&invocation),
                                           xdp_session_dex_get_object_path (speech_session));

  xdp_session_dex_store_take_session (speech->sessions, g_steal_pointer (&session));

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
  g_autoptr (XdpRequestDex) request = NULL;
  XdpSpeechSession *session = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr (GError) error = NULL;
  XdpVoiceProvider *vp;
  const char *provider_name;

  session =
      xdp_session_dex_store_lookup_session (speech->sessions, arg_session_handle, app_info);
  if (!session)
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

  if (!dex_await_boolean (check_speech_permission (speech, app_info, request, arg_parent_window), NULL))
    {
      xdp_request_dex_emit_response (request, XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED, NULL);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  vp = xdp_voice_provider_store_lookup (speech->provider_store, arg_provider_id);
  provider_name = vp ? xdp_voice_provider_get_name (vp) : NULL;
  if (!provider_name)
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  {
    g_auto (GVariantBuilder) request_data_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    g_variant_builder_add (&request_data_builder, "{sv}", "name",
                           g_variant_new_string (provider_name));

    xdp_request_dex_emit_response (request, XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS,
                                   g_variant_builder_end (&request_data_builder));
  }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_get_voices (XdpDbusSpeech         *object,
                   GDBusMethodInvocation *invocation,
                   const char            *arg_session_handle,
                   const char            *arg_parent_window,
                   GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  g_autoptr (XdpRequestDex) request = NULL;
  XdpSpeechSession *session = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  g_autoptr (GError) error = NULL;

  session =
      xdp_session_dex_store_lookup_session (speech->sessions, arg_session_handle, app_info);
  if (!session)
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

  if (!dex_await_boolean (check_speech_permission (speech, app_info, request, arg_parent_window), NULL))
    {
      xdp_request_dex_emit_response (request, XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED, NULL);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  dex_await (xdp_voice_provider_store_wait_ready (speech->provider_store), NULL);

  {
    g_auto (GVariantBuilder) request_data_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

    g_variant_builder_add (&request_data_builder, "{sv}", "voices",
                           speech->voices);

    xdp_request_dex_emit_response (request, XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS,
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
                   double                 arg_pitch,
                   double                 arg_rate,
                   gboolean               arg_is_ssml,
                   const char            *arg_language,
                   GVariant              *arg_options)
{
  XdpSpeech *speech = XDP_SPEECH (object);
  g_autoptr (XdpRequestDex) request = NULL;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
  XdpSpeechSession *session = NULL;
  XdpVoiceProvider *vp;
  g_autoptr (GError) error = NULL;

  session =
      xdp_session_dex_store_lookup_session (speech->sessions, arg_session_handle, app_info);
  if (!session)
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

  xdp_dbus_speech_complete_synthesize (object, g_steal_pointer (&invocation), fd_list,
                                       xdp_request_dex_get_object_path (request));

  if (!dex_await_boolean (check_speech_permission (speech, app_info, request, arg_parent_window), NULL))
    {
      xdp_request_dex_emit_response (request, XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED, NULL);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  dex_await (xdp_voice_provider_store_wait_ready (speech->provider_store), NULL);

  vp = xdp_voice_provider_store_lookup (speech->provider_store, arg_provider_id);
  if (!vp || !xdp_voice_provider_is_ready (vp))
    {
      g_warning ("No provider found");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!dex_await (xdp_voice_provider_ensure_active (vp), &error))
    {
      g_auto (GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      g_variant_builder_add (&builder, "{sv}", "error-message",
                             g_variant_new_string (error->message));
      xdp_request_dex_emit_response (request, XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                     g_variant_builder_end (&builder));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!dex_await (xdp_voice_provider_synthesize (vp,
                                                  arg_pipe_fd, arg_text, arg_voice_id,
                                                  arg_pitch, arg_rate, arg_is_ssml,
                                                  arg_language, fd_list),
                   &error))
    {
      g_auto (GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      g_variant_builder_add (&builder, "{sv}", "error-message",
                             g_variant_new_string (error->message));
      xdp_request_dex_emit_response (request, XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                     g_variant_builder_end (&builder));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_request_dex_emit_response (request, XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS, NULL);

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

  g_clear_pointer (&speech->voices, g_variant_unref);
  g_clear_object (&speech->access_impl);
  g_clear_object (&speech->provider_store);
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
  GDBusConnection *connection = xdp_context_get_connection (context);
  g_autoptr (XdpSpeech) speech = NULL;

  speech = g_object_new (XDP_TYPE_SPEECH, NULL);
  speech->context = context;
  speech->access_impl = access_impl ? g_object_ref (access_impl) : NULL;
  speech->sessions = xdp_session_dex_store_new_wrapped (XdpSpeechSession, session);
  speech->voices = g_variant_ref_sink (
      g_variant_new_array (G_VARIANT_TYPE ("a{sv}"), NULL, 0));

  speech->provider_store = xdp_voice_provider_store_new (connection);
  g_signal_connect_object (speech->provider_store, "changed",
                           G_CALLBACK (on_provider_store_changed), speech, 0);

  xdp_dbus_speech_set_version (XDP_DBUS_SPEECH (speech), 1);

  return g_steal_pointer (&speech);
}

DexFuture *
init_speech (gpointer user_data)
{
  XdpContext *context = XDP_CONTEXT (user_data);
  XdpDbusImplAccess *access_impl;
  g_autoptr (XdpSpeech) speech = NULL;

  if (!xdp_context_is_experimental_portal_enabled (context, "Speech"))
    return dex_future_new_true ();

  g_info ("Enabling experimental Speech portal");

  access_impl = xdp_context_get_access_impl (context);
  if (!access_impl)
    g_warning ("No access backend, speech permission dialogs will not work");

  speech = xdp_speech_new (context, access_impl);

  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&speech)),
                                      XDP_ENTITLEMENT_SPEECH,
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_FIBER);

  return dex_future_new_true ();
}
