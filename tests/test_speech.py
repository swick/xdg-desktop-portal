# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

import os
from pathlib import Path

import dbus
import pytest

import tests.xdp_utils as xdp

SPEECH_PORTAL = "experimental.Speech"
SPEECH_ENTITLEMENT = f"org.freedesktop.portal.{SPEECH_PORTAL}"


@pytest.fixture
def xdp_overwrite_env():
    return {"XDG_DESKTOP_PORTAL_ENABLE_EXPERIMENTAL": "Speech"}


@pytest.fixture(
    params=[
        xdp.AppInfoHost(),
        xdp.AppInfoFlatpak(entitlements=[SPEECH_ENTITLEMENT]),
    ]
)
def xdp_app_info(request) -> xdp.AppInfo:
    return request.param


CACHED_VOICE = (
    "Armenian (West Armenia)",
    "ine/hyw",
    "audio/x-raw,format=S16LE,channels=1,rate=22050",
    dbus.types.UInt64(3),
    ["hyw", "hy-arevmda", "hy"],
)


@pytest.fixture
def required_templates():
    return {
        "speechprovider": {},
        "access": {},
    }


@pytest.fixture
def app_store(create_test_dirs):
    store = xdp.AppStore()
    store.install(
        "org.one.Speech.Provider",
        name="Mock Speech Provider",
        no_display=True,
        implements=["org.freedesktop.Speech.Provider"],
    )
    return store


class SpeechPortalSignalListener:
    def __init__(self, dbus_con):
        self.recieved_portal_signals: dict[str, list[tuple]] = {}

        dbus_con.add_signal_receiver(
            self._signal_reciever,
            dbus_interface=xdp.portal_interface_name(SPEECH_PORTAL),
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
def dbus_signal_reciever(dbus_con):
    dbus_signal_reciever = SpeechPortalSignalListener(dbus_con)
    return dbus_signal_reciever


@pytest.fixture
def speech_provider_mock(dbus_con):
    speechprovider_proxy = dbus_con.get_object(
        "org.one.Speech.Provider",
        "/org/one/Speech/Provider",
    )
    return dbus.Interface(speechprovider_proxy, "org.freedesktop.Speech.Provider.Mock")


@pytest.fixture
def speech_portal(dbus_con):
    return xdp.get_portal_iface(dbus_con, SPEECH_PORTAL)


class TestSpeech:
    @pytest.fixture
    def portals(self, templates, app_store, xdg_desktop_portal, xdg_permission_store):
        return

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

    def test_version(self, portals, app_store, dbus_con):
        xdp.check_version(dbus_con, SPEECH_PORTAL, 1)

    def test_session_get_voices(
        self,
        portals,
        app_store,
        dbus_con,
        dbus_signal_reciever,
        speech_provider_mock,
        speech_portal,
    ):
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

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

        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged",
            lambda: speech_provider_mock.AddVoice(
                "English",
                "gmw/en-US",
                "audio/x-spiel,format=S16LE,channels=1,rate=22050",
                0,
                ["en-us", "en"],
            ),
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 2

        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged", lambda: speech_provider_mock.Hide()
        )

        # Provider is offline but cached voices are still served
        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 2

        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged", lambda: speech_provider_mock.Show()
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 2

        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged",
            lambda: speech_provider_mock.RemoveVoice(0),
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

    def test_session_synthesize(self, portals, app_store, dbus_con, speech_portal):
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

    def test_voice_cache(
        self,
        portals,
        app_store,
        dbus_con,
        dbus_signal_reciever,
        speech_provider_mock,
        speech_portal,
    ):
        """Verify voices are cached in the permission store"""
        permission_store = xdp.get_permission_store_iface(dbus_con)
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 1

        # Verify the permission store has cached the voices
        _, data = permission_store.Lookup("speech-voices", "org.one.Speech.Provider")
        cached_voices = data
        assert len(cached_voices) == 1
        assert cached_voices[0][0] == "Armenian (West Armenia)"
        assert cached_voices[0][1] == "ine/hyw"

        # Add a voice and verify cache is updated
        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged",
            lambda: speech_provider_mock.AddVoice(
                "English",
                "gmw/en-US",
                "audio/x-spiel,format=S16LE,channels=1,rate=22050",
                0,
                ["en-us", "en"],
            ),
        )

        _, data = permission_store.Lookup("speech-voices", "org.one.Speech.Provider")
        cached_voices = data
        assert len(cached_voices) == 2

    def test_get_provider_details_unknown(
        self, portals, app_store, dbus_con, speech_portal
    ):
        """GetProviderDetails for a nonexistent provider returns OTHER"""
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        self.get_provider_details(
            dbus_con,
            speech_portal,
            session,
            "org.nonexistent.Provider",
            expected_response=2,
        )

    def test_synthesize_after_session_close(
        self, portals, app_store, dbus_con, speech_portal
    ):
        """Synthesize on a closed session is rejected"""
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        session.close()

        try:
            self.synthesize(
                dbus_con,
                speech_portal,
                session,
                "org.one.Speech.Provider",
                "gmw/en-US",
            )
        except dbus.exceptions.DBusException as exc:
            assert exc.get_dbus_name() == "org.freedesktop.DBus.Error.AccessDenied"
        else:
            raise AssertionError("No exception was raised")

    def test_multiple_sessions(
        self,
        portals,
        app_store,
        dbus_con,
        dbus_signal_reciever,
        speech_provider_mock,
        speech_portal,
    ):
        """Multiple sessions see the same voices"""
        session1 = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )
        session2 = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token1"}),
        )

        voices1 = self.get_voices(dbus_con, speech_portal, session1)
        voices2 = self.get_voices(dbus_con, speech_portal, session2)
        assert len(voices1) == 1
        assert len(voices2) == 1
        assert voices1[0]["identifier"] == voices2[0]["identifier"]

        # Closing one session doesn't affect the other
        session1.close()

        voices2 = self.get_voices(dbus_con, speech_portal, session2)
        assert len(voices2) == 1


class TestSpeechDeferredActivation:
    """Tests for voice providers that are D-Bus activatable but not yet running.
    The portal should serve cached voices without activating the provider."""

    @pytest.fixture
    def required_templates(self):
        return {"access": {}}

    @pytest.fixture
    def app_store(self, create_test_dirs, create_test_dbus):
        dbus_service_dir = Path(create_test_dbus.get_services_dir())
        store = xdp.AppStore(dbus_service_dir=dbus_service_dir)
        store.install(
            "org.one.Speech.Provider",
            name="Mock Speech Provider",
            template="speechprovider",
            implements=["org.freedesktop.Speech.Provider"],
        )
        return store

    @pytest.fixture
    def voice_cache(self, dbus_con, xdg_permission_store):
        """Seed the permission store with cached voices."""
        permission_store = xdp.get_permission_store_iface(dbus_con)
        permission_store.SetValue(
            "speech-voices",
            True,
            "org.one.Speech.Provider",
            dbus.Array([CACHED_VOICE], signature="(ssstas)"),
        )

    @pytest.fixture
    def xdg_desktop_portal_options(self, voice_cache):
        """Ensure voice cache is seeded before the portal starts."""
        return xdp.PortalProcessOptions()

    @pytest.fixture
    def portals(self, templates, app_store, xdg_desktop_portal, xdg_permission_store):
        return

    @pytest.fixture
    def speech_portal(self, dbus_con):
        return xdp.get_portal_iface(dbus_con, SPEECH_PORTAL)

    @pytest.fixture
    def dbus_signal_reciever(self, dbus_con):
        return SpeechPortalSignalListener(dbus_con)

    def get_voices(self, dbus_con, speech_portal, session):
        get_voices_request = xdp.Request(dbus_con, speech_portal)
        get_voices_response = get_voices_request.call(
            "GetVoices",
            session_handle=session.handle,
            parent_window="window-hndl",
            options={},
        )
        assert get_voices_response
        assert get_voices_response.response == 0
        return get_voices_response.results.get("voices", [])

    def test_cached_voices_without_activation(self, portals, dbus_con, speech_portal):
        """Cached voices are served without activating the provider"""
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        # Provider is not running
        assert not dbus_con.name_has_owner("org.one.Speech.Provider")

        # Voices are served from cache
        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 1
        assert voices[0]["name"] == "Armenian (West Armenia)"
        assert voices[0]["identifier"] == "ine/hyw"

        # Provider was not activated by GetVoices
        assert not dbus_con.name_has_owner("org.one.Speech.Provider")

    def test_synthesize_activates_provider(self, portals, dbus_con, speech_portal):
        """Synthesize triggers D-Bus activation of the provider"""
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        assert not dbus_con.name_has_owner("org.one.Speech.Provider")

        readfd, writefd = os.pipe()
        synthesize_request = xdp.Request(dbus_con, speech_portal)
        synthesize_response = synthesize_request.call(
            "Synthesize",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id="org.one.Speech.Provider",
            pipe_fd=dbus.types.UnixFd(writefd),
            text="HELLO world",
            voice_id="ine/hyw",
            pitch=10,
            rate=10,
            is_ssml=False,
            language="en",
            options={},
        )

        assert synthesize_response
        assert synthesize_response.response == 0

        f = os.fdopen(readfd, "r")
        assert f.read(11) == "hello WORLD"
        f.close()

        # Provider was activated by synthesize
        assert dbus_con.name_has_owner("org.one.Speech.Provider")

    def test_provider_self_start_updates_client_and_cache(
        self,
        portals,
        dbus_con,
        dbus_signal_reciever,
        speech_portal,
    ):
        """When a not-yet-activated provider starts on its own and emits
        voices changed, the portal updates both client and cache"""
        permission_store = xdp.get_permission_store_iface(dbus_con)
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        # Voices from cache
        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 1

        # Provider was not activated
        assert not dbus_con.name_has_owner("org.one.Speech.Provider")

        # Activate the provider by calling a method on it directly
        provider_obj = dbus_con.get_object(
            "org.one.Speech.Provider",
            "/org/one/Speech/Provider",
        )
        provider_mock = dbus.Interface(
            provider_obj, "org.freedesktop.Speech.Provider.Mock"
        )

        # Provider adds a voice after starting
        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged",
            lambda: provider_mock.AddVoice(
                "English",
                "gmw/en-US",
                "audio/x-spiel,format=S16LE,channels=1,rate=22050",
                0,
                ["en-us", "en"],
            ),
        )

        # Client sees the updated voice list
        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 2

        # Cache is updated with the new voice
        _, data = permission_store.Lookup("speech-voices", "org.one.Speech.Provider")
        assert len(data) == 2


class TestSpeechEntitlements:
    @pytest.fixture
    def xdp_app_info(self) -> xdp.AppInfo:
        return xdp.AppInfoFlatpak(entitlements=[])

    @pytest.fixture
    def portals(self, templates, app_store, xdg_desktop_portal, xdg_permission_store):
        return

    def test_not_allowed(self, portals, dbus_con):
        speech_portal = xdp.get_portal_iface(dbus_con, SPEECH_PORTAL)

        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            speech_portal.CreateSession({})
        assert (
            excinfo.value.get_dbus_name() == "org.freedesktop.portal.Error.NotAllowed"
        )
