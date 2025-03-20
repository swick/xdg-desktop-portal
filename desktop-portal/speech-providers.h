/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include <gio/gunixfdlist.h>
#include <libdex.h>

#define XDP_TYPE_VOICE_PROVIDER (xdp_voice_provider_get_type ())
G_DECLARE_FINAL_TYPE (XdpVoiceProvider,
                      xdp_voice_provider,
                      XDP, VOICE_PROVIDER,
                      GObject)

const char * xdp_voice_provider_get_bus_name (XdpVoiceProvider *self);

const char * xdp_voice_provider_get_name (XdpVoiceProvider *self);

GVariant * xdp_voice_provider_get_voices (XdpVoiceProvider *self);

gboolean xdp_voice_provider_is_ready (XdpVoiceProvider *self);

DexFuture * xdp_voice_provider_ensure_active (XdpVoiceProvider *self);

DexFuture * xdp_voice_provider_synthesize (XdpVoiceProvider *self,
                                           GVariant         *pipe_fd,
                                           const char       *text,
                                           const char       *voice_id,
                                           double            pitch,
                                           double            rate,
                                           gboolean          is_ssml,
                                           const char       *language,
                                           GUnixFDList      *fd_list);

#define XDP_TYPE_VOICE_PROVIDER_STORE (xdp_voice_provider_store_get_type ())
G_DECLARE_FINAL_TYPE (XdpVoiceProviderStore,
                      xdp_voice_provider_store,
                      XDP, VOICE_PROVIDER_STORE,
                      GObject)

XdpVoiceProviderStore * xdp_voice_provider_store_new (GDBusConnection *connection);

DexFuture * xdp_voice_provider_store_wait_ready (XdpVoiceProviderStore *store);

XdpVoiceProvider * xdp_voice_provider_store_lookup (XdpVoiceProviderStore *store,
                                                    const char            *provider_id);

GList * xdp_voice_provider_store_get_providers (XdpVoiceProviderStore *store);
