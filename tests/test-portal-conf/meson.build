test_portals = [
  'org.freedesktop.impl.portal.Access',
  'org.freedesktop.impl.portal.Account',
  'org.freedesktop.impl.portal.AppChooser',
  'org.freedesktop.impl.portal.Background',
  'org.freedesktop.impl.portal.Clipboard',
  'org.freedesktop.impl.portal.Email',
  'org.freedesktop.impl.portal.FileChooser',
  'org.freedesktop.impl.portal.GlobalShortcuts',
  'org.freedesktop.impl.portal.Inhibit',
  'org.freedesktop.impl.portal.InputCapture',
  'org.freedesktop.impl.portal.Lockdown',
  'org.freedesktop.impl.portal.Notification',
  'org.freedesktop.impl.portal.Print',
  'org.freedesktop.impl.portal.RemoteDesktop',
  'org.freedesktop.impl.portal.Screenshot',
  'org.freedesktop.impl.portal.Settings',
  'org.freedesktop.impl.portal.Wallpaper',
]

test_portal_conf = configuration_data()
test_portal_conf.set('PORTALS', ';'.join(test_portals))

test_portal = configure_file(input: 'test.portal.in',
    output: 'test.portal',
    configuration: test_portal_conf,
    install: enable_installed_tests,
    install_dir: installed_tests_dir / 'portals/test',
)

configure_file(input: 'test-portals.conf',
    output: '@PLAINNAME@',
    copy: true,
    install: enable_installed_tests,
    install_dir: installed_tests_dir / 'portals/test',
)
