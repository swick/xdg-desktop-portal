# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

import os
import subprocess
from contextlib import contextmanager
from pathlib import Path

import dbus
import dbusmock
import pytest

import tests.xdp_utils as xdp

_TESTS_DIR = Path(__file__).resolve().parent


@contextmanager
def _chdir(path):
    old = os.getcwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(old)


class SpeechProviderApp:
    """
    A mock speech provider application. Manages a desktop file with the
    Implements key for intent-based discovery and a dbusmock process
    that implements the org.freedesktop.Speech.Provider interface.
    """

    BUS_NAME = "org.one.Speech.Provider"
    OBJ_PATH = "/org/freedesktop/Speech/Provider"
    MAIN_IFACE = "org.freedesktop.Speech.Provider"
    MOCK_IFACE = "org.freedesktop.Speech.Provider.Mock"

    def __init__(self, dbus_con):
        self._dbus_con = dbus_con
        self._process = None
        self._mock = None
        self._service_file = None
        self._apps_dir = Path(os.environ["XDG_DATA_HOME"]) / "applications"
        self._desktop_file = self._apps_dir / f"{self.BUS_NAME}.desktop"
        self._intent_cache = self._apps_dir / "intent.cache"

    def install(self):
        self._apps_dir.mkdir(parents=True, exist_ok=True)
        self._desktop_file.write_text(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Mock Speech Provider\n"
            "Exec=/bin/true\n"
            f"Implements={self.MAIN_IFACE};\n"
        )
        self._intent_cache.write_text(
            f"[Intent Cache]\n{self.MAIN_IFACE}={self.BUS_NAME}.desktop;\n"
        )

        services_dir = Path(dbusmock.DBusTestCase.get_services_dir())
        template_path = (_TESTS_DIR / "templates" / "speechprovider.py").absolute()
        project_root = _TESTS_DIR.parent.absolute()
        self._service_file = services_dir / f"{self.BUS_NAME}.service"
        self._service_file.write_text(
            f"[D-BUS Service]\n"
            f"Name={self.BUS_NAME}\n"
            f"Exec=/bin/sh -c 'cd {project_root} && exec python3 -m dbusmock -l /dev/null -t {template_path}'\n"
        )

    def uninstall(self):
        self._desktop_file.unlink(missing_ok=True)
        self._intent_cache.unlink(missing_ok=True)
        if self._service_file:
            self._service_file.unlink(missing_ok=True)
            self._service_file = None

    def start(self):
        assert self._process is None

        template_path = str((_TESTS_DIR / "templates" / "speechprovider.py").absolute())

        self._process = subprocess.Popen(
            [
                "python3",
                "-m",
                "dbusmock",
                "--session",
                "-t",
                template_path,
            ],
            stdout=None,
            stderr=None,
            cwd=str(_TESTS_DIR.parent),
        )

        xdp.wait_for(lambda: self._dbus_con.name_has_owner(self.BUS_NAME))
        self._mock = self._dbus_con.get_object(self.BUS_NAME, self.OBJ_PATH)

    def stop(self):
        if self._process:
            self._process.terminate()
            self._process.wait()
            self._process = None
        elif self._dbus_con.name_has_owner(self.BUS_NAME):
            dbus_obj = self._dbus_con.get_object(
                "org.freedesktop.DBus", "/org/freedesktop/DBus"
            )
            pid = dbus_obj.GetConnectionUnixProcessID(
                self._dbus_con.get_name_owner(self.BUS_NAME),
                dbus_interface="org.freedesktop.DBus",
            )
            os.kill(pid, 15)
        self._mock = None

    @property
    def mock(self):
        if self._mock is None:
            self._mock = self._dbus_con.get_object(self.BUS_NAME, self.OBJ_PATH)
        return dbus.Interface(self._mock, self.MOCK_IFACE)


class SpeechPermissions:
    def __init__(self, dbus_con, app_id):
        self.app_id = app_id
        self.permissions_store = xdp.get_permission_store_iface(dbus_con)

    def set_permission(self, perm):
        self.permissions_store.SetPermission(
            "speech",
            True,
            "speech",
            self.app_id,
            [perm],
        )


class SpeechPortalSignalListener:
    def __init__(self, dbus_con):
        self.recieved_portal_signals: dict[str, list[tuple]] = {}

        dbus_con.add_signal_receiver(
            self._signal_reciever,
            dbus_interface=xdp.portal_interface_name("Speech"),
            member_keyword="member",
        )

    def _signal_reciever(self, *args, **kwargs):
        member = kwargs["member"]
        self.recieved_portal_signals[member] = self.recieved_portal_signals.get(
            member, []
        ) + [args]

    def wait_for_portal_signal(self, signal_name, invoker):
        recieved_count = len(self.recieved_portal_signals.get(signal_name, []))

        invoker()

        xdp.wait_for(
            lambda: (
                len(self.recieved_portal_signals.get(signal_name, [])) > recieved_count
            )
        )
        return self.recieved_portal_signals[signal_name].pop()


@pytest.fixture
def required_templates():
    return {
        "access": {},
    }


@pytest.fixture
def speech_provider_app(dbus_con):
    app = SpeechProviderApp(dbus_con)
    app.install()
    yield app
    app.stop()
    app.uninstall()


@pytest.fixture
def portals(speech_provider_app, templates, xdg_desktop_portal, xdg_permission_store):
    return


@pytest.fixture
def speech_permissions(xdg_permission_store, xdp_app_info, dbus_con):
    speech_permissions = SpeechPermissions(dbus_con, xdp_app_info.app_id)
    speech_permissions.set_permission("yes")
    return speech_permissions


@pytest.fixture
def speech_portal(dbus_con):
    return xdp.get_portal_iface(dbus_con, "Speech")


class TestSpeech:
    def get_provider_details(
        self, dbus_con, speech_portal, session, provider_id, expected_response=0
    ):
        get_provider_details_request = xdp.Request(dbus_con, speech_portal)
        get_provider_details_response = get_provider_details_request.call(
            "GetProviderDetails",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id=provider_id,
            options={},
        )

        assert get_provider_details_response
        assert get_provider_details_response.response == expected_response
        return get_provider_details_response.results

    def get_voices(self, dbus_con, speech_portal, session, expected_response=0):
        get_voices_request = xdp.Request(dbus_con, speech_portal)
        get_voices_response = get_voices_request.call(
            "GetVoices",
            session_handle=session.handle,
            parent_window="window-hndl",
            options={},
        )

        assert get_voices_response
        assert get_voices_response.response == expected_response
        return get_voices_response.results.get("voices", [])

    def synthesize(
        self,
        dbus_con,
        speech_portal,
        session,
        provider_id,
        voice_id,
        expected_response=0,
    ):
        readfd, writefd = os.pipe()
        synthesize_request = xdp.Request(dbus_con, speech_portal)
        synthesize_response = synthesize_request.call(
            "Synthesize",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id=provider_id,
            pipe_fd=dbus.types.UnixFd(writefd),
            text="HELLO world",
            voice_id=voice_id,
            pitch=10,
            rate=10,
            is_ssml=False,
            language="en",
            options={},
        )

        assert synthesize_response
        assert synthesize_response.response == expected_response

        if expected_response != 0:
            return

        f = os.fdopen(readfd, "r")

        # XXX: Write stream should be closed so we should be able to read to EOF but that is not the case.
        assert f.read(11) == "hello WORLD"
        f.close()

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Speech", 1)

    def test_session_get_voices(
        self,
        portals,
        dbus_con,
        speech_provider_app,
        speech_permissions,
        speech_portal,
    ):
        signal_listener = SpeechPortalSignalListener(dbus_con)

        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        # The provider was auto-started during initial discovery (no voice cache)
        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 1

        assert voices[0]["name"] == "Armenian (West Armenia)"
        assert voices[0]["identifier"] == "ine/hyw"
        assert (
            voices[0]["output-format"]
            == "audio/x-raw,format=S16LE,channels=1,rate=22050"
        )
        assert voices[0]["features"] == 3
        assert voices[0]["languages"] == ["hyw", "hy-arevmda", "hy"]
        assert voices[0]["provider"] == "org.one.Speech.Provider"

        provider_details = self.get_provider_details(
            dbus_con, speech_portal, session, "org.one.Speech.Provider"
        )
        assert provider_details["name"] == "Mock Speech Provider"

        signal_listener.wait_for_portal_signal(
            "VoicesChanged",
            lambda: speech_provider_app.mock.AddVoice(
                "English",
                "gmw/en-US",
                "audio/x-spiel,format=S16LE,channels=1,rate=22050",
                0,
                ["en-us", "en"],
            ),
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 2

        # Stop the provider app — cached voices remain available
        speech_provider_app.stop()
        xdp.wait_for(lambda: not dbus_con.name_has_owner(speech_provider_app.BUS_NAME))

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 2

        # Restart the provider app — voices refresh from the running provider
        signal_listener.wait_for_portal_signal(
            "VoicesChanged", lambda: speech_provider_app.start()
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 1

        # Close session
        session.close()

        try:
            self.get_voices(dbus_con, speech_portal, session)
        except dbus.exceptions.DBusException as exc:
            assert exc.get_dbus_name() == "org.freedesktop.DBus.Error.AccessDenied"
        else:
            raise AssertionError("No assertion was raised")

    def test_session_synthesize(
        self,
        portals,
        dbus_con,
        speech_provider_app,
        speech_permissions,
        speech_portal,
    ):
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        self.synthesize(
            dbus_con, speech_portal, session, "org.one.Speech.Provider", "gmw/en-US"
        )

        # Test synthesizing using unknown provider
        self.synthesize(
            dbus_con, speech_portal, session, "foo", "gmw/en-US", expected_response=2
        )

    def test_access_denied(
        self,
        portals,
        dbus_con,
        speech_provider_app,
        speech_permissions,
        speech_portal,
    ):
        speech_permissions.set_permission("no")

        signal_listener = SpeechPortalSignalListener(dbus_con)

        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        self.get_voices(dbus_con, speech_portal, session, expected_response=1)

        self.get_provider_details(
            dbus_con,
            speech_portal,
            session,
            "org.one.Speech.Provider",
            expected_response=1,
        )

        self.synthesize(
            dbus_con,
            speech_portal,
            session,
            "org.one.Speech.Provider",
            "gmw/en-US",
            expected_response=1,
        )

        # Allowing the app will cause a providers changed signal
        signal_listener.wait_for_portal_signal(
            "VoicesChanged", lambda: speech_permissions.set_permission("yes")
        )

        voices = self.get_voices(
            dbus_con,
            speech_portal,
            session,
        )
        assert len(voices) == 1
