template_files = [
  '__init__.py',
  'account.py',
  'background.py',
  'clipboard.py',
  'email.py',
  'geoclue2.py',
  'globalshortcuts.py',
  'inputcapture.py',
  'remotedesktop.py',
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
