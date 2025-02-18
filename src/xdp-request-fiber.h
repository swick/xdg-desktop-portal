/*
 * Copyright © 2025 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "xdp-app-info.h"
#include "xdp-utils.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef enum {
  XDP_REQUEST_RESPONSE_SUCCESS = 0,
  XDP_REQUEST_RESPONSE_CANCELLED,
  XDP_REQUEST_RESPONSE_OTHER,
} XdpRequestResponse;

struct _XdpRequestFiberClass
{
  XdpDbusRequestSkeletonClass parent_class;
};

#define XDP_TYPE_REQUEST_FIBER (xdp_request_fiber_get_type())
G_DECLARE_FINAL_TYPE (XdpRequestFiber,
                      xdp_request_fiber,
                      XDP, REQUEST_FIBER,
                      XdpDbusRequestSkeleton)

XdpRequestFiber * xdp_request_fiber_new (GDBusMethodInvocation  *invocation,
                                         XdpAppInfo             *app_info,
                                         const char             *token,
                                         const char             *dbus_name,
                                         GError                **error);

XdpRequestFiber * xdp_request_fiber_new_from_options (GDBusMethodInvocation  *invocation,
                                                      XdpAppInfo             *app_info,
                                                      GVariant               *options,
                                                      const char             *dbus_name,
                                                      GError                **error);

const char * xdp_request_fiber_get_path (XdpRequestFiber *request);

void xdp_request_fiber_set_response (XdpRequestFiber    *request,
                                     XdpRequestResponse  response,
                                     GVariant           *results);

gboolean xdp_request_fiber_close (XdpRequestFiber  *request,
                                  GError          **error);

