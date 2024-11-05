# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import dbus
from gi.repository import GLib, Gio


class TestPermissionStore:
    def get_permission_store_obj(self, dbus_con):
        try:
            obj = getattr(self, "_xdp_permission_store")
        except AttributeError:
            obj = dbus_con.get_object(
                "org.freedesktop.impl.portal.PermissionStore",
                "/org/freedesktop/impl/portal/PermissionStore",
            )
            assert obj
            self._xdp_permission_store = obj
        return obj

    def get_permission_store_intf(self, dbus_con):
        return dbus.Interface(
            self.get_permission_store_obj(dbus_con),
            "org.freedesktop.impl.portal.PermissionStore",
        )

    def get_proxy(self):
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        return Gio.DBusProxy.new_sync(
            bus,
            Gio.DBusProxyFlags.NONE,
            None,
            "org.freedesktop.impl.portal.PermissionStore",
            "/org/freedesktop/impl/portal/PermissionStore",
            "org.freedesktop.impl.portal.PermissionStore",
            None,
        )

    def Set(self, table, create, id, perm, data):
        proxy = self.get_proxy()
        return proxy.call_sync(
            "Set",
            GLib.Variant("(sbsa{sas}v)", (table, create, id, perm, data)),
            0,
            -1,
            None,
        )

    def SetPermissionAsync(self, table, create, id, app, perm, user_cb):
        def cb(s, res, _):
            res = s.call_finish(res)
            user_cb(res)

        proxy = self.get_proxy()
        proxy.call(
            "SetPermission",
            GLib.Variant("(sbssas)", (table, create, id, app, perm)),
            0,
            -1,
            None,
            cb,
            None,
        )

    def DeletePermissionAsync(self, table, id, app, user_cb):
        def cb(s, res, _):
            res = s.call_finish(res)
            user_cb(res)

        proxy = self.get_proxy()
        proxy.call(
            "DeletePermission",
            GLib.Variant("(sss)", (table, id, app)),
            0,
            -1,
            None,
            cb,
            None,
        )

    def DeleteAsync(self, table, id, user_cb):
        def cb(s, res, _):
            res = s.call_finish(res)
            user_cb(res)

        proxy = self.get_proxy()
        proxy.call(
            "Delete",
            GLib.Variant("(ss)", (table, id)),
            0,
            -1,
            None,
            cb,
            None,
        )

    def test_version(self, portals, dbus_con):
        permission_store = self.get_permission_store_obj(dbus_con)

        properties_intf = dbus.Interface(
            permission_store,
            "org.freedesktop.DBus.Properties",
        )
        portal_version = properties_intf.Get(
            "org.freedesktop.impl.portal.PermissionStore",
            "version",
        )
        assert int(portal_version) == 2

    def test_delete_race(self, portals, dbus_con):
        permission_store_intf = self.get_permission_store_intf(dbus_con)
        mainloop = GLib.MainLoop()
        finished_count = 0

        table = "inhibit"
        id = "inhibit"
        perms = ["logout", "suspend"]

        def cb(_):
            nonlocal finished_count

            finished_count += 1

        self.SetPermissionAsync(table, True, id, "a", perms, cb)
        self.DeleteAsync(table, id, cb)

        while finished_count < 2:
            GLib.timeout_add(50, mainloop.quit)
            mainloop.run()

        try:
            permission_store_intf.Lookup(table, id)
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

        self.SetPermissionAsync(table, True, id, "a", perms, cb)
        self.SetPermissionAsync(table, True, id, "b", perms, cb)
        self.DeletePermissionAsync(table, id, "a", cb)

        while finished_count < 4:
            GLib.timeout_add(50, mainloop.quit)
            mainloop.run()

        perms_out, _ = permission_store_intf.Lookup(table, id)
        assert perms_out == {"b": perms}

        self.SetPermissionAsync(table, True, id, "a", perms, cb)
        self.DeletePermissionAsync(table, id, "b", cb)
        self.DeletePermissionAsync(table, id, "a", cb)

        while finished_count < 7:
            GLib.timeout_add(50, mainloop.quit)
            mainloop.run()

        perms_out, _ = permission_store_intf.Lookup(table, id)
        assert perms_out == {}

    def test_change(self, portals, dbus_con):
        permission_store_intf = self.get_permission_store_intf(dbus_con)
        mainloop = GLib.MainLoop()
        changed_count = 0

        table = "TEST"
        id = "test-resource"
        app = "one.two.three"
        perms = ["one", "two"]

        def cb_changed1(cb_table, cb_id, deleted, cb_data, cb_perms):
            nonlocal changed_count

            assert cb_table == table
            assert cb_id == id
            assert not deleted
            assert cb_perms[app] == perms

            changed_count += 1

        cs = permission_store_intf.connect_to_signal("Changed", cb_changed1)

        permission_store_intf.SetPermission(table, True, id, app, perms)

        while changed_count < 1:
            GLib.timeout_add(50, mainloop.quit)
            mainloop.run()

        def cb_changed2(cb_table, cb_id, deleted, cb_data, cb_perms):
            nonlocal changed_count

            assert cb_table == table
            assert cb_id == id
            assert deleted

            changed_count += 1

        GLib.timeout_add(1000, mainloop.quit)
        mainloop.run()

        cs.remove()
        cs = permission_store_intf.connect_to_signal("Changed", cb_changed2)
        permission_store_intf.Delete(table, id)

        while changed_count < 2:
            GLib.timeout_add(50, mainloop.quit)
            mainloop.run()

    def test_lookup(self, portals, dbus_con):
        permission_store_intf = self.get_permission_store_intf(dbus_con)

        table = "TEST"
        id = "test-resource"
        perms = ["one", "two"]
        data = True

        try:
            permission_store_intf.Lookup(table, id)
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

        permissions = [(id, perms)]
        self.Set(table, True, id, permissions, GLib.Variant("b", data))

        perms_out, data_out = permission_store_intf.Lookup(table, id)
        assert id in perms_out
        perms_out = perms_out[id]
        assert perms_out == perms

        assert data_out == data

    def test_set_value(self, portals, dbus_con):
        permission_store_intf = self.get_permission_store_intf(dbus_con)

        table = "TEST"
        id = "test-resource"
        data = True

        try:
            permission_store_intf.Lookup(table, id)
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

        permission_store_intf.SetValue(table, True, id, data)

        perms_out, data_out = permission_store_intf.Lookup(table, id)
        assert perms_out == {}
        assert data_out == data

    def test_create(self, portals, dbus_con):
        permission_store_intf = self.get_permission_store_intf(dbus_con)

        table = "inhibit"
        id = "inhibit"
        app = ""
        perms = ["logout", "suspend"]

        try:
            permission_store_intf.SetPermission(
                table,
                # Do not create if it does not exist
                False,
                id,
                app,
                perms,
            )
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

        permission_store_intf.SetPermission(table, True, id, app, perms)

    def test_delete(self, portals, dbus_con):
        permission_store_intf = self.get_permission_store_intf(dbus_con)

        table = "inhibit"
        id = "inhibit"
        app = ""
        perms = ["logout", "suspend"]

        try:
            permission_store_intf.Delete(table, id)
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

        permission_store_intf.SetPermission(table, True, id, app, perms)

        permission_store_intf.Delete(table, id)

        try:
            permission_store_intf.Lookup(table, id)
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

    def test_get_permission(self, portals, dbus_con):
        permission_store_intf = self.get_permission_store_intf(dbus_con)

        table = "notifications"
        id = "notification"
        app = "a"
        perms = ["yes"]

        try:
            permission_store_intf.GetPermission(table, id, app)
            assert False, "This statement should not be reached"
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.NotFound"

        permission_store_intf.SetPermission(table, True, id, app, perms)

        permissions = permission_store_intf.GetPermission(table, id, app)
        assert permissions == perms

        permissions = permission_store_intf.GetPermission(table, id, "no-such-app")
        assert permissions == []
