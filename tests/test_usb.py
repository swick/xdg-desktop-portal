# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests import Session
from gi.repository import GLib

import gi
gi.require_version('UMockdev', '1.0')
from gi.repository import UMockdev

import dbus
import pytest


@pytest.fixture
def portal_name():
    return "Usb"


@pytest.fixture
def umockdev():
    return UMockdev.Testbed.new()


class TestUsb:
    def test_version(self, portal_mock):
        portal_mock.check_version(1)


    def test_create_close_session(self, portal_mock, app_id):
        usb_intf = portal_mock.get_dbus_interface()

        session = Session(
            portal_mock.dbus_con,
            usb_intf.CreateSession({"session_handle_token": "session_token0"})
        )

        session.close()


    def test_empty_initial_devices(self, portal_mock, app_id):
        device_events_signal_received = False

        usb_intf = portal_mock.get_dbus_interface()

        session = Session(
            portal_mock.dbus_con,
            usb_intf.CreateSession({"session_handle_token": "session_token0"})
        )

        def cb_device_events(session_handle, events):
            nonlocal device_events_signal_received
            device_events_signal_received = True

        usb_intf.connect_to_signal("DeviceEvents", cb_device_events)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        assert not device_events_signal_received

    @pytest.mark.parametrize('usb_queries', ["vnd:04a9", None])
    def test_initial_devices(self, portal_mock, app_id, usb_queries):
        device_events_signal_received = False
        devices_received = 0
        testbed = portal_mock.umockdev

        testbed.add_from_string(
r'''P: /devices/pci0000:00/0000:00:1a.0/usb1/1-1/1-1.5/1-1.5.2/1-1.5.2.3
N: bus/usb/001/011=1201000200000040A904C03102000102030109022700010100C001090400000306010100070581020002000705020200020007058303080009
E: BUSNUM=001
E: COLORD_DEVICE=1
E: COLORD_KIND=camera
E: DEVNAME=/dev/bus/usb/001/011
E: DEVNUM=011
E: DEVTYPE=usb_device
E: DRIVER=usb
E: GPHOTO2_DRIVER=PTP
E: ID_BUS=usb
E: ID_GPHOTO2=1
E: ID_MODEL=Canon_Digital_Camera
E: ID_MODEL_ENC=Canon\x20Digital\x20Camera
E: ID_MODEL_ID=31c0
E: ID_REVISION=0002
E: ID_SERIAL=Canon_Inc._Canon_Digital_Camera_C767F1C714174C309255F70E4A7B2EE2
E: ID_SERIAL_SHORT=C767F1C714174C309255F70E4A7B2EE2
E: ID_USB_INTERFACES=:060101:
E: ID_VENDOR=Canon_Inc.
E: ID_VENDOR_ENC=Canon\x20Inc.
E: ID_VENDOR_ID=04a9
E: MAJOR=189
E: MINOR=10
E: PRODUCT=4a9/31c0/2
E: SUBSYSTEM=usb
E: TAGS=:udev-acl:
E: TYPE=0/0/0
A: authorized=1
A: avoid_reset_quirk=0
A: bConfigurationValue=1
A: bDeviceClass=00
A: bDeviceProtocol=00
A: bDeviceSubClass=00
A: bMaxPacketSize0=64
A: bMaxPower=  2mA
A: bNumConfigurations=1
A: bNumInterfaces= 1
A: bcdDevice=0002
A: bmAttributes=c0
A: busnum=1\n
A: configuration=
H: descriptors=1201000200000040A904C03102000102030109022700010100C001090400000306010100070581020002000705020200020007058303080009
A: dev=189:10
A: devnum=11\n
A: devpath=1.5.2.3
A: idProduct=31c0
A: idVendor=04a9
A: manufacturer=Canon Inc.
A: maxchild=0
A: product=Canon Digital Camera
A: quirks=0x0
A: removable=unknown
A: serial=C767F1C714174C309255F70E4A7B2EE2
A: speed=480
A: urbnum=43
A: version= 2.00
''')

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        usb_intf = portal_mock.get_dbus_interface()

        session = Session(
            portal_mock.dbus_con,
            usb_intf.CreateSession({"session_handle_token": "session_token0"})
        )

        def cb_device_events(session_handle, events):
            nonlocal device_events_signal_received
            nonlocal devices_received

            device_events_signal_received = True
            assert session.handle == session_handle

            for action, id, device in events:
                assert action == "add"
                devices_received += 1

        usb_intf.connect_to_signal("DeviceEvents", cb_device_events)

        mainloop = GLib.MainLoop()
        GLib.timeout_add(300, mainloop.quit)
        mainloop.run()

        if usb_queries == None:
            assert not device_events_signal_received
            assert devices_received == 0
        else:
            assert device_events_signal_received
            assert devices_received == 1
