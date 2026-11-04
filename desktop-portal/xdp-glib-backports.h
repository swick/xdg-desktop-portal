/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#pragma once

#include "config.h"

#include <gio/gdesktopappinfo.h>

#if !HAVE_GIO_INTENT_API

GAppInfo **xdp_g_desktop_app_info_get_for_intent (const char *interface,
                                                  const char *scope,
                                                  size_t     *out_n_results);

gboolean xdp_g_desktop_app_info_has_intent (GDesktopAppInfo *info,
                                            const char      *intent);

#define g_desktop_app_info_get_for_intent xdp_g_desktop_app_info_get_for_intent
#define g_desktop_app_info_has_intent xdp_g_desktop_app_info_has_intent

#endif /* !HAVE_GIO_INTENT_API */
