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

#include <gio/gio.h>

#define XDP_TYPE_DESKTOP_PORTAL (xdp_desktop_portal_get_type())
G_DECLARE_FINAL_TYPE (XdpDesktopPortal,
                      xdp_desktop_portal,
                      XDP, DESKTOP_PORTAL,
                      GObject)

typedef struct _XdpPortalImpls XdpPortalImpls;
typedef struct _XdpDbusImplLockdown XdpDbusImplLockdown;
typedef struct _XdpDbusImplAccess XdpDbusImplAccess;

XdpDesktopPortal * xdp_desktop_portal_new (gboolean opt_verbose);

gboolean xdp_desktop_portal_register (XdpDesktopPortal  *desktop_portal,
                                      GDBusConnection   *connection,
                                      GError           **error);

GDBusConnection * xdp_desktop_portal_get_connection (XdpDesktopPortal *desktop_portal);

XdpPortalImpls * xdp_desktop_portal_get_impls (XdpDesktopPortal *desktop_portal);

XdpDbusImplLockdown * xdp_desktop_portal_get_lockdown_proxy (XdpDesktopPortal *desktop_portal);

XdpDbusImplAccess * xdp_desktop_portal_get_access_proxy (XdpDesktopPortal *desktop_portal);

gboolean xdp_desktop_portal_export (XdpDesktopPortal        *desktop_portal,
                                    GDBusInterfaceSkeleton  *skeleton,
                                    GError                 **error);

gboolean xdp_desktop_portal_export_host (XdpDesktopPortal        *desktop_portal,
                                         GDBusInterfaceSkeleton  *skeleton,
                                         GError                 **error);

gboolean xdp_desktop_portal_is_verbose (XdpDesktopPortal *desktop_portal);
