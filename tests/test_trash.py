# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import os
import pytest
import tempfile
import dbus
from pathlib import Path


@pytest.fixture
def xdp_app_info_entitlements():
    return ["org.freedesktop.portal.Trash.Foo"]


class TestTrash:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Trash", 1)

    @pytest.fixture(params=[xdp.AppInfoKind.HOST, xdp.AppInfoKind.FLATPAK])
    def test_trash_file_fails(self, portals, dbus_con):
        trash_intf = xdp.get_portal_iface(dbus_con, "Trash")
        try:
            with open("/proc/cmdline") as fd:
                result = trash_intf.TrashFile(fd.fileno())
        except PermissionError as e:
            pytest.skip(f"Couldn't open file /proc/cmdline: {e}")

        assert result == 0

    @pytest.mark.parametrize(
        "xdp_app_info_entitlements",
        (
            None,
            ["org.freedesktop.portal.Trash.Foo"],
            [],
        ),
    )
    def test_trash_file(
        self, portals, dbus_con, xdp_app_info, xdp_app_info_entitlements
    ):
        trash_intf = xdp.get_portal_iface(dbus_con, "Trash")

        fd, name = tempfile.mkstemp(prefix="trash_portal_mock_", dir=Path.home())

        try:
            result = trash_intf.TrashFile(fd)
        except dbus.exceptions.DBusException as e:
            # not allowed when strict is enabled and the entitlement is not set
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotAllowed"
            assert xdp_app_info_entitlements == []
            assert xdp_app_info.kind in {xdp.AppInfoKind.FLATPAK, xdp.AppInfoKind.HOST}
            return

        if result != 1:
            os.unlink(name)
        assert result == 1
        assert not Path(name).exists()
