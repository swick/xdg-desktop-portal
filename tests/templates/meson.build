template_files = [
  '__init__.py',
  'access.py',
  'account.py',
  'appchooser.py',
  'background.py',
  'clipboard.py',
  'email.py',
  'geoclue2.py',
  'globalshortcuts.py',
  'inhibit.py',
  'inputcapture.py',
  'lockdown.py',
  'notification.py',
  'print.py',
  'remotedesktop.py',
  'screenshot.py',
  'wallpaper.py',
]
foreach template_file : template_files
  configure_file(
    input: template_file,
    output: template_file,
    copy: true,
    install: false
  )
endforeach
