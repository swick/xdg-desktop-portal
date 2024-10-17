# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates import Response, init_template_logger, ImplRequest, ImplSession
import dbus
import dbus.service
from dbusmock import MOCK_IFACE

from gi.repository import GLib


BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Usb"
VERSION = 1


logger = init_template_logger(__name__)


def load(mock, parameters={}):
    logger.debug(f"Loading parameters: {parameters}")

    mock.delay: int = parameters.get("delay", 200)
    mock.response: int = parameters.get("response", 0)
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "version": dbus.UInt32(parameters.get("version", VERSION)),
            }
        ),
    )


@dbus.service.method(
    MAIN_IFACE,
    in_signature="ossa(sa{sv}a{sv})a{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def AcquireDevices(
    self,
    handle,
    parent_window,
    app_id,
    devices,
    options,
    cb_success,
    cb_error,
):
    try:
        logger.debug(
            f"AcquireDevices({handle}, {parent_window}, {app_id}, {devices}, {options})"
        )

        # no options supported
        assert not options
        devices_out = []

        for device in devices:
            (id, info, access_options) = device

            devices_out.append(dbus.Struct(
                [
                    id,
                    access_options
                ],
                signature="sa{sv}", variant_level=1
            ))

        response = Response(self.response, {"devices": devices_out})
        request = ImplRequest(self, BUS_NAME, handle)
        request.export()

        def reply():
            logger.debug(f"AcquireDevices with response {response}")
            cb_success(response.response, response.results)

        logger.debug(f"scheduling delay of {self.delay}")
        GLib.timeout_add(self.delay, reply)

    except Exception as e:
        logger.critical(e)
        cb_error(e)
