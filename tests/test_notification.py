# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import dbus
import pytest


@pytest.fixture
def required_templates():
    return {"notification": {}}


class TestNotification:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Notification", 2)

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
