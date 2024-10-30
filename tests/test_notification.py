# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import dbus
import pytest
import tempfile
import os
from pathlib import Path
from gi.repository import GLib, Gio

SVG_IMAGE_DATA = """<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" height="16px" width="16px"/>
"""

SOUND_DATA = (
    b"\x52\x49\x46\x46\x24\x00\x00\x00\x57\x41\x56\x45"
    + b"\x66\x6d\x74\x20\x10\x00\x00\x00\x01\x00\x01\x00"
    + b"\x44\xac\x00\x00\x88\x58\x01\x00\x02\x00\x10\x00"
    + b"\x64\x61\x74\x61\x00\x00\x00\x00"
)


SUPPORTED_OPTIONS = {
    "foo": "bar",
}


@pytest.fixture
def required_templates():
    return {
        "notification": {
            "SupportedOptions": SUPPORTED_OPTIONS,
        },
    }


class TestNotification:
    def add_notification(self, id, notification, fds=[]):
        # This uses Gio functionality to make the dbus call
        # because this allows us to specify the types accurately
        # using GVariant.
        # This is only used when python-dbus does not work.

        fdlist = Gio.UnixFDList.new()
        for fd in fds:
            fdlist.append(fd)
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        proxy = Gio.DBusProxy.new_sync(
            bus,
            Gio.DBusProxyFlags.NONE,
            None,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Notification",
            None,
        )
        res, outfdlist = proxy.call_with_unix_fd_list_sync(
            "AddNotification",
            GLib.Variant("(sa{sv})", (id, notification)),
            0,
            -1,
            fdlist,
            None,
        )

    def check_notification(
        self, dbus_con, app_id, id, notification_in, notification_expected
    ):
        email_intf = xdp.get_portal_iface(dbus_con, "Notification")
        mock_intf = xdp.get_mock_iface(dbus_con)

        method_calls = mock_intf.GetMethodCalls("AddNotification")
        backend_calls = len(method_calls)

        email_intf.AddNotification(id, notification_in)

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("AddNotification")
        assert len(method_calls) == backend_calls + 1
        _, args = method_calls[-1]
        assert args[0] == app_id
        assert args[1] == id

        mock_notification = args[2]
        assert len(notification_expected) == len(mock_notification)
        for k in notification_expected.keys():
            assert notification_expected[k] == mock_notification[k]

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Notification", 2)

    def test_notification_basic(self, portals, dbus_con, app_id):
        notification = {
            "title": "title",
            "body": "test notification body",
            "priority": "normal",
            "default-action": "test-action",
        }
        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

    def test_notification_remove(self, portals, dbus_con, app_id):
        email_intf = xdp.get_portal_iface(dbus_con, "Notification")
        mock_intf = xdp.get_mock_iface(dbus_con)

        id = "test1"
        notification = {
            "title": "title",
            "body": "test notification body",
            "priority": "normal",
            "default-action": "test-action",
        }

        email_intf.AddNotification(id, notification)
        method_calls = mock_intf.GetMethodCalls("AddNotification")
        assert len(method_calls) == 1
        _, args = method_calls[-1]
        assert args[0] == app_id
        assert args[1] == id

        email_intf.RemoveNotification(id)
        method_calls = mock_intf.GetMethodCalls("RemoveNotification")
        assert len(method_calls) == 1
        _, args = method_calls[-1]
        assert args[0] == app_id
        assert args[1] == id

    def test_notification_buttons(self, portals, dbus_con, app_id):
        notification = {
            "title": "test notification 2",
            "body": "test notification body 2",
            "priority": "low",
            "default-action": "test-action",
            "buttons": dbus.Array(
                [
                    {
                        "label": "button1",
                        "action": "action1",
                    },
                    {
                        "label": "button2",
                        "action": "action2",
                    },
                ],
                signature="a{sv}",
            ),
        }
        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

    def test_notification_markup(self, portals, dbus_con, app_id):
        notification_base = {
            "title": "title",
            "priority": "normal",
            "default-action": "test-action",
        }
        bodies = [
            (
                "test <b>notification</b> body <i>italic</i>",
                "test <b>notification</b> body <i>italic</i>",
            ),
            (
                'test <a href="https://example.com"><b>Some link</b></a>',
                'test <a href="https://example.com"><b>Some link</b></a>',
            ),
            (
                "test \n newline \n\n some more space \n  with trailing space ",
                "test newline some more space with trailing space",
            ),
            (
                "test <custom> tag </custom>",
                "test tag",
            ),
            (
                "test <b>notification<b> body",
                False,
            ),
            (
                "<b>foo<i>bar</b></i>",
                False,
            ),
            (
                "test <markup><i>notification</i><markup> body",
                False,
            ),
        ]

        i = 0
        for body_in, body_expected in bodies:
            notification_in = notification_base.copy()
            notification_in["markup-body"] = body_in

            notification_expected = notification_base.copy()
            notification_expected["markup-body"] = body_expected

            try:
                self.check_notification(
                    dbus_con,
                    app_id,
                    f"test{i}",
                    notification_in,
                    notification_expected,
                )
                assert body_expected
            except dbus.exceptions.DBusException:
                assert not body_expected

            i += 1

    def test_notification_bad_arg(self, portals, dbus_con, app_id):
        notification = {
            "title": "title",
            "bodx": "test notification body",
        }
        notification_expected = {
            "title": "title",
        }
        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification_expected,
        )

    def test_notification_bad_priority(self, portals, dbus_con, app_id):
        notification = {
            "title": "test notification 2",
            "body": "test notification body 2",
            "priority": "invalid",
        }
        try:
            self.check_notification(
                dbus_con,
                app_id,
                "test1",
                notification,
                notification,
            )
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException:
            pass

    def test_notification_bad_button(self, portals, dbus_con, app_id):
        notification = {
            "title": "test notification 2",
            "body": "test notification body 2",
            "buttons": dbus.Array(
                [
                    {
                        "labex": "button1",
                        "action": "action1",
                    },
                    {
                        "label": "button2",
                        "action": "action2",
                    },
                ],
                signature="a{sv}",
            ),
        }
        try:
            self.check_notification(
                dbus_con,
                app_id,
                "test1",
                notification,
                notification,
            )
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException:
            pass

    def test_notification_display_hint(self, portals, dbus_con, app_id):
        notification = {
            "title": "title",
            "body": "test notification body",
            "display-hint": ["transient", "show-as-new"],
        }
        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

        notification = {
            "title": "title",
            "body": "test notification body",
            "display-hint": ["unsupported-hint"],
        }
        try:
            self.check_notification(
                dbus_con,
                app_id,
                "test1",
                notification,
                notification,
            )
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException:
            pass

    def test_notification_category(self, portals, dbus_con, app_id):
        notification = {
            "title": "title",
            "body": "test notification body",
            "category": "im.received",
        }
        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

        notification = {
            "title": "title",
            "body": "test notification body",
            "category": "x-vendor.custom",
        }
        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

        notification = {
            "title": "title",
            "body": "test notification body",
            "category": "unsupported-type",
        }
        try:
            self.check_notification(
                dbus_con,
                app_id,
                "test1",
                notification,
                notification,
            )
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException:
            pass

    def test_supported_options(self, portals, dbus_con, app_id):
        properties_intf = xdp.get_iface(dbus_con, "org.freedesktop.DBus.Properties")

        options = properties_intf.Get(
            "org.freedesktop.portal.Notification", "SupportedOptions"
        )

        assert options == SUPPORTED_OPTIONS

    def test_icon_themed(self, portals, dbus_con, app_id):
        icon = Gio.ThemedIcon.new("test-icon-symbolic")

        notification = {
            "title": GLib.Variant("s", "title"),
            "icon": icon.serialize(),
        }

        self.add_notification("test1", notification)

    def test_icon_bytes(self, portals, dbus_con, app_id):
        bytes = GLib.Bytes.new(SVG_IMAGE_DATA.encode("utf-8"))
        icon = Gio.BytesIcon.new(bytes)

        notification = {
            "title": GLib.Variant("s", "title"),
            "icon": icon.serialize(),
        }

        self.add_notification("test1", notification)

    def test_icon_file(self, portals, dbus_con, app_id):
        fd, file_path = tempfile.mkstemp(prefix="notification_icon_", dir=Path.home())
        os.write(fd, SVG_IMAGE_DATA.encode("utf-8"))

        file = Gio.File.new_for_path(file_path)
        icon = Gio.FileIcon.new(file)

        notification = {
            "title": GLib.Variant("s", "title"),
            "icon": icon.serialize(),
        }

        self.add_notification("test1", notification)

    def test_icon_bad(self, portals, dbus_con, app_id):
        notification = {
            "title": GLib.Variant("s", "title"),
        }

        bad_icons = [
            GLib.Variant("(sv)", ["themed", GLib.Variant("s", "test-icon-symbolic")]),
            GLib.Variant(
                "(sv)",
                ["bytes", GLib.Variant("as", ["test-icon-symbolic", "test-icon"])],
            ),
            GLib.Variant("(sv)", ["file-descriptor", GLib.Variant("s", "")]),
            GLib.Variant("(sv)", ["file-descriptor", GLib.Variant("h", 0)]),
        ]

        for icon in bad_icons:
            notification["icon"] = icon
            try:
                self.add_notification("test1", notification)
                assert False, "This statement should not be reached"
            except GLib.GError as e:
                assert e.matches(Gio.io_error_quark(), Gio.IOErrorEnum.DBUS_ERROR)
                pass

    def test_sound_simple(self, portals, dbus_con, app_id):
        notification = {
            "title": "title",
            "body": "test notification body",
            "sound": "default",
            "default-action": "test-action",
        }
        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

        notification = {
            "title": "title",
            "body": "test notification body",
            "sound": "silent",
            "default-action": "test-action",
        }
        self.check_notification(
            dbus_con,
            app_id,
            "test1",
            notification,
            notification,
        )

        notification = {
            "title": "title",
            "body": "test notification body",
            "sound": "bad",
            "default-action": "test-action",
        }
        try:
            self.check_notification(
                dbus_con,
                app_id,
                "test1",
                notification,
                notification,
            )
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException:
            pass

    def test_sound_file(self, portals, dbus_con, app_id):
        mock_intf = xdp.get_mock_iface(dbus_con)

        fd, file_path = tempfile.mkstemp(prefix="notification_sound_", dir=Path.home())
        os.write(fd, SOUND_DATA)

        file = Gio.File.new_for_path(file_path)

        notification = {
            "title": GLib.Variant("s", "title"),
            "sound": GLib.Variant("(sv)", ["file", GLib.Variant("s", file.get_uri())]),
        }

        self.add_notification("test1", notification)

        method_calls = mock_intf.GetMethodCalls("AddNotification")
        assert len(method_calls) == 1
        _, args = method_calls[-1]
        mock_notification = args[2]

        assert "sound" not in mock_notification

    def test_sound_fd(self, portals, dbus_con, app_id):
        mock_intf = xdp.get_mock_iface(dbus_con)

        fd = os.memfd_create("notification_sound_test", os.MFD_ALLOW_SEALING)
        os.write(fd, SOUND_DATA)

        notification = {
            "title": GLib.Variant("s", "title"),
            "sound": GLib.Variant("(sv)", ["file-descriptor", GLib.Variant("h", 0)]),
        }

        self.add_notification("test1", notification, [fd])

        method_calls = mock_intf.GetMethodCalls("AddNotification")
        assert len(method_calls) == 1
        _, args = method_calls[-1]
        mock_notification = args[2]

        assert mock_notification["sound"][0] == "file-descriptor"
        mock_fd = mock_notification["sound"][1]
        mock_fd = mock_fd.take()

        os.lseek(fd, 0, os.SEEK_SET)
        fd_contents = os.read(mock_fd, 1000)
        assert fd_contents == SOUND_DATA

        os.close(mock_fd)
        os.close(fd)

    def test_sound_bad(self, portals, dbus_con, app_id):
        notification = {
            "title": GLib.Variant("s", "title"),
        }

        bad_sounds = [
            # bad type
            GLib.Variant("(sv)", ["file-descriptor", GLib.Variant("s", "")]),
            # not sending the FD for the handle
            GLib.Variant("(sv)", ["file-descriptor", GLib.Variant("h", 13)]),
        ]

        for sound in bad_sounds:
            notification["sound"] = sound
            try:
                self.add_notification("test1", notification)
                assert False, "This statement should not be reached"
            except GLib.GError as e:
                assert e.matches(Gio.io_error_quark(), Gio.IOErrorEnum.DBUS_ERROR)
                pass
