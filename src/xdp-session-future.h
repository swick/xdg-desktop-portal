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

#pragma once

#include "xdp-dbus.h"
#include "xdp-types.h"

#define XDP_TYPE_SESSION_FUTURE (xdp_session_future_get_type ())
G_DECLARE_FINAL_TYPE (XdpSessionFuture,
                      xdp_session_future,
                      XDP, SESSION_FUTURE,
                      XdpDbusSessionSkeleton)

DexFuture * xdp_session_future_new (XdpContext             *context,
                                    XdpAppInfo             *app_info,
                                    GDBusInterfaceSkeleton *skeleton,
                                    GDBusProxy             *proxy_impl,
                                    GVariant               *arg_options);

gboolean xdp_session_future_is_closed (XdpSessionFuture *session);

XdpAppInfo * xdp_session_future_get_app_info (XdpSessionFuture *session);

const char * xdp_session_future_get_object_path (XdpSessionFuture *session);
