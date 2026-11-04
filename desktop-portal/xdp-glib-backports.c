/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "xdp-glib-backports.h"

#if !HAVE_GIO_INTENT_API

GAppInfo **
xdp_g_desktop_app_info_get_for_intent (const char *interface,
                                        const char *scope,
                                        size_t     *out_n_results)
{
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  GList *impls = g_desktop_app_info_get_implementations (interface);
  G_GNUC_END_IGNORE_DEPRECATIONS
  size_t n = g_list_length (impls);

  if (out_n_results)
    *out_n_results = n;

  if (n == 0)
    {
      g_list_free (impls);
      return NULL;
    }

  GAppInfo **result = g_new (GAppInfo *, n + 1);
  size_t i = 0;

  for (GList *l = impls; l; l = l->next)
    result[i++] = l->data;

  result[n] = NULL;
  g_list_free (impls);

  return result;
}

gboolean
xdp_g_desktop_app_info_has_intent (GDesktopAppInfo *info,
                                    const char      *intent)
{
  g_auto (GStrv) implements = NULL;

  g_return_val_if_fail (G_IS_DESKTOP_APP_INFO (info), FALSE);

  implements = g_desktop_app_info_get_string_list (info, "Implements", NULL);

  return implements != NULL &&
         g_strv_contains ((const char * const *) implements, intent);
}

#endif /* !HAVE_GIO_INTENT_API */
