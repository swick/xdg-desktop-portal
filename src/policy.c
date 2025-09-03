entitlement
flatpak override --user --add-policy=portal.openuri.deeplink-origins=open.spotify.com org.gnome.clocks
portal.Policy.Ask('portal.openuri.deeplink-origins')
 -> Access impl -> permission store
portal.Policy.GrantedPolicies = ['portal.openuri.deeplink-origins']
portal.OpenURI... ()
 -> Desktop file has X-Flatpak -> check for portal.openuri.deeplink-origins in permission store
