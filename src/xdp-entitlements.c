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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <string.h>

#include "xdp-entitlements.h"

#include "xdp-utils.h"

typedef struct _XdpEntitlements
{
  GHashTable *entitlements;
} XdpEntitlements;

static const char *entitlements_list[] =
{
  "deeplink.origins",
};

static const char *entitlements_boolean[] =
{
};

XdpEntitlements *
xdp_entitlements_new (void)
{
  XdpEntitlements *et = g_new0 (XdpEntitlements, 1);

  et->entitlements = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free,
                                            (GDestroyNotify) g_ptr_array_unref);

  return et;
}

void
xdp_entitlements_free (XdpEntitlements *et)
{
  g_clear_pointer (&et->entitlements, g_hash_table_unref);
  g_free (et);
}

static void
xdp_entitlements_add_list_internal (XdpEntitlements *et,
                                    const char      *entitlement_id,
                                    GStrv            list)
{
  g_autofree char *owned_id = NULL;
  g_autoptr(GPtrArray) owned_list = NULL;

  if (!g_hash_table_steal_extended (et->entitlements, entitlement_id,
                                    (gpointer *) &owned_id,
                                    (gpointer *) &owned_list))
    {
      owned_id = g_strdup (entitlement_id);
      owned_list = g_ptr_array_new_null_terminated (0, g_free, TRUE);
    }

  for (size_t i = 0; list[i] != NULL; i++)
    {
      const char *e = list[i];

      if (g_ptr_array_find_with_equal_func (owned_list, e, g_str_equal, NULL))
        continue;

      g_ptr_array_add (owned_list, g_strdup (e));
    }

  if (owned_list->len == 0)
    return;

  g_hash_table_insert (et->entitlements,
                       g_steal_pointer (&owned_id),
                       g_steal_pointer (&owned_list));
}

static gboolean
xdp_entitlements_add_list (XdpEntitlements  *et,
                           const char       *entitlement_id,
                           GStrv             list,
                           GError          **error)
{
  if (!g_strv_contains (entitlements_list, entitlement_id))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unsupported entitlement");
      return FALSE;
    }

  xdp_entitlements_add_list_internal (et, entitlement_id, list);
  return TRUE;
}

static void
xdp_entitlements_add_boolean_internal (XdpEntitlements *et,
                                       const char      *entitlement_id)
{
  g_hash_table_insert (et->entitlements,
                       g_strdup (entitlement_id),
                       NULL);
}

static gboolean
xdp_entitlements_add_boolean (XdpEntitlements  *et,
                              const char       *entitlement_id,
                              GError          **error)
{
  if (g_strv_contains (entitlements_boolean, entitlement_id))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unsupported entitlement");
      return FALSE;
    }

  xdp_entitlements_add_boolean_internal (et, entitlement_id);
  return TRUE;
}

void
xdp_entitlements_add (XdpEntitlements *et,
                      const char      *entitlement_id,
                      GStrv            list)
{
  if (g_strv_contains (entitlements_list, entitlement_id))
    {
      xdp_entitlements_add_list_internal (et, entitlement_id, list);
      return;
    }

  if (g_strv_contains (entitlements_boolean, entitlement_id))
    {
      if (g_strv_contains ((const gchar * const *) list, "enabled"))
        xdp_entitlements_add_boolean_internal (et, entitlement_id);
      return;
    }

  g_debug ("Unsupported entitlement %s", entitlement_id);
}

gboolean
xdp_entitlements_lookup (XdpEntitlements *et,
                         const char      *entitlement_id)
{
  return g_hash_table_contains (et->entitlements, entitlement_id);
}

GStrv
xdp_entitlements_lookup_list (XdpEntitlements *et,
                              const char      *entitlement_id)
{
  GPtrArray *list;

  list = g_hash_table_lookup (et->entitlements, entitlement_id);
  if (!list)
    return FALSE;

  return (const GStrv) list->pdata;
}

GVariant *
xdp_entitlements_serialize (XdpEntitlements *et)
{
  g_auto(GVariantBuilder) builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  GHashTableIter iter;
  const char *entitlement_id;
  GPtrArray *list;

  g_hash_table_iter_init (&iter, et->entitlements);
  while (g_hash_table_iter_next (&iter,
                                 (gpointer *) &entitlement_id,
                                 (gpointer *) &list))
    {
      GVariant *value = NULL;

      if (list)
        {
          g_auto(GVariantBuilder) list_builder =
            G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_STRING_ARRAY);

          for (size_t i = 0; i < list->len; i++)
            g_variant_builder_add (&list_builder, "s", list->pdata[i]);

          value = g_variant_builder_end (&list_builder);
        }
      else
        {
          value = g_variant_new_boolean (TRUE);
        }

      g_variant_builder_add (&builder, "{sv}", entitlement_id, value);
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

XdpEntitlements *
xdp_entitlements_deserialize (GVariant  *entitlements_variant,
                              GError   **error)
{
  g_autoptr(XdpEntitlements) et = xdp_entitlements_new ();
  GVariantIter iter;
  const char *entitlement_id;
  GVariant *value;

  if (!g_variant_is_of_type (entitlements_variant, G_VARIANT_TYPE_VARDICT))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "GVariant is not a vardict");
      return NULL;
    }

  g_variant_iter_init (&iter, entitlements_variant);
  while (g_variant_iter_next (&iter, "{&sv}", &entitlement_id, &value))
    {
      g_autoptr(GVariant) entitlement = value;
      g_autoptr(GError) local_error = NULL;

      if (g_variant_is_of_type (entitlement, G_VARIANT_TYPE_BOOLEAN))
        {
          if (!xdp_entitlements_add_boolean (et, entitlement_id, &local_error))
            g_debug ("Deserializing %s failed: %s", entitlement_id, local_error->message);
        }
      else if (g_variant_is_of_type (entitlement, G_VARIANT_TYPE_STRING_ARRAY))
        {
          g_autofree const char **list = NULL;

          list = g_variant_get_strv (entitlement, NULL);
          if (!xdp_entitlements_add_list (et, entitlement_id, (GStrv) list, &local_error))
            g_debug ("Deserializing %s failed: %s", entitlement_id, local_error->message);
        }
      else
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Vardict value is neither a string array, nor a boolean");
          return NULL;
        }
    }

  return NULL;
}

XdpEntitlements *
xdp_entitlements_intersect (XdpEntitlements *et1,
                            XdpEntitlements *et2)
{
  g_autoptr(XdpEntitlements) et_intersect = xdp_entitlements_new ();
  GHashTableIter iter;
  const char *entitlement_id;
  GPtrArray *list1;

  g_hash_table_iter_init (&iter, et1->entitlements);
  while (g_hash_table_iter_next (&iter,
                                 (gpointer *) &entitlement_id,
                                 (gpointer *) &list1))
    {
      GPtrArray *list2;
      g_autoptr(GPtrArray) intersection_list = NULL;

      /* not in intersection */
      if (!g_hash_table_lookup_extended (et2->entitlements,
                                         entitlement_id,
                                         NULL,
                                         (gpointer *) &list2))
        continue;

      /* boolean entitlement */
      if (list1 == NULL || list2 == NULL)
        {
          g_assert (list1 == NULL && list2 == NULL);

          g_hash_table_insert (et_intersect->entitlements,
                               g_strdup (entitlement_id),
                               NULL);
          continue;
        }

      /* list entitlements */
      intersection_list = g_ptr_array_new_null_terminated (0, g_free, TRUE);

      for (size_t i = 0; i < list1->len; i++)
        {
          if (!g_ptr_array_find_with_equal_func (list2,
                                                 list1->pdata[i],
                                                 g_str_equal, NULL))
            continue;

          g_ptr_array_add (intersection_list, g_strdup (list1->pdata[i]));
        }

      if (intersection_list->len == 0)
        continue;

      g_hash_table_insert (et_intersect->entitlements,
                           g_strdup (entitlement_id),
                           g_steal_pointer (&intersection_list));
    }

  return g_steal_pointer (&et_intersect);
}
