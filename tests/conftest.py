# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black
#
# Environment variables:
#   G_TEST_BUILDDIR: the path to the tests/ build dir
#   XDP_DBUS_MONITOR: if set, starts dbus_monitor on the custom bus, useful
#                     for debugging
#
# Make sure the required portals are listed in test-portal-conf/meson.build
# and you have a dbusmock template for the impl.portal of your portal in
# tests/templates. See the dbusmock documentation for details on those
# templates.

from typing import Any, Dict

import pytest
import dbus
import dbusmock
import os
import sys
import tempfile
import subprocess
import fcntl
import time
import signal
from pathlib import Path


def pytest_configure():
    ensure_environment_set()
    ensure_umockdev_loaded()


def ensure_environment_set():
    if not os.getenv("G_TEST_BUILDDIR"):
        raise Exception("G_TEST_BUILDDIR must be set")


def ensure_umockdev_loaded():
    umockdev_preload = "libumockdev-preload.so"
    preload = os.environ.get("LD_PRELOAD", "")
    if umockdev_preload not in preload:
        os.environ["LD_PRELOAD"] = f"{umockdev_preload}:{preload}"
        os.execv(sys.executable, [sys.executable] + sys.argv)


@pytest.fixture(autouse=True)
def create_test_dirs(umockdev):
    # The umockdev argument is to make sure the testbed
    # is created before we create the tmpdir
    env_dirs = [
        "HOME",
        "TMPDIR",
        "XDG_CACHE_HOME",
        "XDG_CONFIG_HOME",
        "XDG_DATA_HOME",
        "XDG_RUNTIME_DIR",
    ]

    test_root = tempfile.TemporaryDirectory(
        prefix="xdp-testroot-", ignore_cleanup_errors=True
    )

    for env_dir in env_dirs:
        directory = Path(test_root.name) / env_dir.lower()
        directory.mkdir(mode=0o700, parents=True)
        os.environ[env_dir] = str(directory.absolute())

    yield test_root

    test_root.cleanup()


@pytest.fixture(autouse=True)
def create_test_dbus():
    bus = dbusmock.DBusTestCase()
    bus.setUp()
    bus.start_session_bus()
    bus.start_system_bus()

    yield bus

    bus.tearDown()
    bus.tearDownClass()


@pytest.fixture(autouse=True)
def create_dbus_monitor():
    if not os.getenv("XDP_DBUS_MONITOR"):
        yield None
        return

    dbus_monitor = subprocess.Popen(["dbus-monitor", "--session"])

    yield dbus_monitor

    dbus_monitor.terminate()
    dbus_monitor.wait()


def _get_server_for_module(busses, module, bustype):
    assert bustype in dbusmock.BusType

    try:
        return busses[bustype][module.BUS_NAME]
    except KeyError:
        server = dbusmock.SpawnedMock.spawn_for_name(
            module.BUS_NAME,
            "/dbusmock",
            dbusmock.OBJECT_MANAGER_IFACE,
            bustype,
            stdout=subprocess.PIPE,
        )

        flags = fcntl.fcntl(server.process.stdout, fcntl.F_GETFL)
        fcntl.fcntl(server.process.stdout, fcntl.F_SETFL, flags | os.O_NONBLOCK)

        busses[bustype][module.BUS_NAME] = server
        return server


def _get_main_obj_for_module(server, module, bustype):
    try:
        server.obj.AddObject(
            module.MAIN_OBJ,
            "com.example.EmptyInterface",
            {},
            [],
            dbus_interface=dbusmock.MOCK_IFACE,
        )
    except Exception:
        pass

    bustype.wait_for_bus_object(module.BUS_NAME, module.MAIN_OBJ)
    bus = bustype.get_connection()
    return bus.get_object(module.BUS_NAME, module.MAIN_OBJ)


def _terminate_mock_p(process):
    if process.stdout:
        out = (process.stdout.read() or b"").decode("utf-8")
        if out:
            print(out)
        process.stdout.close()
    process.terminate()
    process.wait()


def _terminate_servers(busses):
    for server in busses[dbusmock.BusType.SYSTEM].values():
        _terminate_mock_p(server.process)
    for server in busses[dbusmock.BusType.SESSION].values():
        _terminate_mock_p(server.process)


def _start_template(busses, template: str, params: Dict[str, Any] = {}):
    """
    Start the template and potentially start a server for it
    """

    template = f"tests/templates/{template.lower()}.py"
    module = dbusmock.mockobject.load_module(template)
    bustype = dbusmock.BusType.SYSTEM if module.SYSTEM_BUS else dbusmock.BusType.SESSION

    server = _get_server_for_module(busses, module, bustype)
    main_obj = _get_main_obj_for_module(server, module, bustype)

    main_obj.AddTemplate(
        template,
        dbus.Dictionary(params, signature="sv"),
        dbus_interface=dbusmock.MOCK_IFACE,
    )


@pytest.fixture
def template_params() -> dict[str, dict[str, Any]]:
    """
    Default fixture for overriding the parameters which should be passed to the
    mocking templates. Use required_templates to specify the default parameters
    and override it for specific test cases via

        @pytest.mark.parametrize("template_params", ({"Template": {"foo": "bar"}},))

    """
    return {}


@pytest.fixture
def required_templates() -> dict[str, dict[str, Any]]:
    """
    Default fixture for enumerating the mocking templates the test case requires
    to be started. This is a map from a name of a template in the templates
    directory to the parameters which should be passed to the template.
    """
    return {}


@pytest.fixture
def templates(required_templates, template_params):
    busses = {dbusmock.BusType.SYSTEM: {}, dbusmock.BusType.SESSION: {}}
    for template, params in required_templates.items():
        params = template_params.get(template, params)
        _start_template(busses, template, params)
    yield
    _terminate_servers(busses)


@pytest.fixture
def xdp_overwrite_env():
    return {}


@pytest.fixture
def app_id():
    return "org.example.Test"


@pytest.fixture
def xdp_env(umockdev, app_id, xdp_overwrite_env):
    env = os.environ.copy()
    env["G_DEBUG"] = "fatal-criticals"
    env["XDG_CURRENT_DESKTOP"] = "test"
    env["XDG_DESKTOP_PORTAL_TEST_APP_ID"] = app_id

    if umockdev:
        env["UMOCKDEV_DIR"] = umockdev.get_root_dir()

    portal_dir = Path(os.getenv("G_TEST_BUILDDIR")) / "test-portal-conf"
    if not portal_dir.exists():
        raise FileNotFoundError(f"{portal_dir} does not exist")
    env["XDG_DESKTOP_PORTAL_DIR"] = portal_dir

    asan_suppression = Path(os.getenv("G_TEST_BUILDDIR", "tests")) / "asan.suppression"
    if not asan_suppression.exists():
        raise FileNotFoundError(f"{asan_suppression} does not exist")
    env["LSAN_OPTIONS"] = f"suppressions={asan_suppression}"

    for key, val in xdp_overwrite_env.items():
        env[key] = val

    return env


def _maybe_add_asan_preload(executable, env):
    # ASAN really wants to be the first library to get loaded but we also
    # LD_PRELOAD umockdev and LD_PRELOAD gets loaded before any "normally"
    # linked libraries. This uses ldd to find the version of libasan.so that
    # should be loaded and puts it in front of LD_PRELOAD.
    # This way, LD_PRELOAD and ASAN can be used at the same time.
    ldd = subprocess.check_output(["ldd", executable]).decode("utf-8")
    libs = [line.split()[0] for line in ldd.splitlines()]
    try:
        libasan = next(filter(lambda lib: lib.startswith("libasan"), libs))
    except StopIteration:
        return

    preload = env.get("LD_PRELOAD", "")
    env["LD_PRELOAD"] = f"{libasan}:{preload}"


@pytest.fixture
def xdg_desktop_portal_path():
    return Path(os.getenv("G_TEST_BUILDDIR")) / ".." / "src" / "xdg-desktop-portal"


@pytest.fixture
def xdg_desktop_portal(dbus_con, xdg_desktop_portal_path, xdp_env):
    if not xdg_desktop_portal_path.exists():
        raise FileNotFoundError(f"{xdg_desktop_portal_path} does not exist")

    env = xdp_env.copy()
    _maybe_add_asan_preload(xdg_desktop_portal_path, env)

    xdg_desktop_portal = subprocess.Popen([xdg_desktop_portal_path], env=env)

    for _ in range(50):
        if dbus_con.name_has_owner("org.freedesktop.portal.Desktop"):
            break
        time.sleep(0.1)
    else:
        assert False, "Timeout while waiting for xdg-desktop-portal to claim the bus"

    yield xdg_desktop_portal

    xdg_desktop_portal.send_signal(signal.SIGHUP)
    returncode = xdg_desktop_portal.wait()
    assert returncode == 0


@pytest.fixture
def xdg_permission_store_path():
    return (
        Path(os.getenv("G_TEST_BUILDDIR"))
        / ".."
        / "document-portal"
        / "xdg-permission-store"
    )


@pytest.fixture
def xdg_permission_store(dbus_con, xdg_permission_store_path, xdp_env):
    if not xdg_permission_store_path.exists():
        raise FileNotFoundError(f"{xdg_permission_store_path} does not exist")

    env = xdp_env.copy()
    _maybe_add_asan_preload(xdg_permission_store_path, env)

    permission_store = subprocess.Popen([xdg_permission_store_path], env=env)

    for _ in range(50):
        if dbus_con.name_has_owner("org.freedesktop.impl.portal.PermissionStore"):
            break
        time.sleep(0.1)
    else:
        assert False, "Timeout while waiting for xdg-permission-store to claim the bus"

    yield permission_store

    permission_store.send_signal(signal.SIGHUP)
    permission_store.wait()
    # The permission store does not shut down cleanly currently
    # returncode = permission_store.wait()
    # assert returncode == 0


@pytest.fixture
def portals(templates, xdg_desktop_portal, xdg_permission_store):
    return None


@pytest.fixture
def umockdev():
    return None


@pytest.fixture
def xdg_data_home_files():
    return {}


@pytest.fixture(autouse=True)
def ensure_xdg_data_home(create_test_dirs, xdg_data_home_files):
    for name, content in xdg_data_home_files.items():
        file_path = Path(os.environ["XDG_DATA_HOME"]) / name
        file_path.parent.mkdir(parents=True, exist_ok=True)
        with open(str(file_path.absolute()), "w") as f:
            f.write(content)


@pytest.fixture
def dbus_con(create_test_dbus):
    con = create_test_dbus.get_dbus(system_bus=False)
    assert con
    return con


@pytest.fixture
def dbus_con_sys(create_test_dbus):
    con_sys = create_test_dbus.get_dbus(system_bus=True)
    assert con_sys
    return con_sys
