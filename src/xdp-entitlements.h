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

#pragma once

/*
 * Flatpak Metadata:
 *
 *   [Policy entitlement]
 *   deeplink.origins=open.spotify.com;music.youtube.com
 *   accessibility.screen-reader=enabled
 *
 * Flatpak commands
 *
 *   --add-policy=entitlement.deeplink.origins=open.spotify.com
 *   --add-policy=entitlement.deeplink.origins=music.youtube.com
 *   --remove-policy=entitlement.deeplink.origins=open.spotify.com
 *
 * To keep things predictable, each entitlement id (e.g. `deeplink.origins`,
 * `accessibility.screen-reader`) has either a boolean or a list of abstract
 * strings as value.
 *
 * The app gets told which entitlements were granted, including the string
 * values.
 *
 * A boolean entitlement is either granted or not granted.
 * A list of strings entitlement can grant a subset of the strings requested,
 * when another set of strings was previously granted.
 *
 * The impl must inform the frontend about which entitlement id's are supported
 * because it somehow must inform the user about the consequences of the
 * entitlement.
 *
 * The permission store stores for each app id a vardict of entitlements with
 * the id being the key, and either a boolean or array of strings (`as`) as
 * value.
 *
 * - Frontend loads Metadata
 * - Frontend loads Permission
 * - Frontend builds intersection
 *   + Intersection is used for decision making
 *   + Portal uses intersection to tell the client about granted entitlements
 * - Entitlement Portal gets called
 *   + Provides entitlements to grant
 *   + Frontend builds intersection of Metadata and provided entitlements
 *   + Intersection is sent to the impl
 *   + Impl adjusts values in the Permission Store
 *  - Frontend listens for Permission Store changes
 */

#include <glib.h>

typedef struct _XdpEntitlements XdpEntitlements;

XdpEntitlements * xdp_entitlements_new (void);

void xdp_entitlements_free (XdpEntitlements *entitlements);

void xdp_entitlements_add (XdpEntitlements *et,
                           const char      *entitlement_id,
                           GStrv            list);

GVariant * xdp_entitlements_serialize (XdpEntitlements *et);

XdpEntitlements * xdp_entitlements_deserialize (GVariant  *entitlements,
                                                GError   **error);

XdpEntitlements * xdp_entitlements_intersect (XdpEntitlements *et1,
                                              XdpEntitlements *et2);

gboolean xdp_entitlements_lookup (XdpEntitlements *et,
                                  const char      *entitlement_id);

GStrv xdp_entitlements_lookup_list (XdpEntitlements *et,
                                    const char      *entitlement_id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpEntitlements, xdp_entitlements_free)
