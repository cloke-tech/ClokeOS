#!/usr/bin/env python3
"""Legt die Programme von ClokeOS als .app-Bundles an, aufgebaut wie bei macOS:

  Name.app/Contents/Info.plist          CFBundleName, CFBundleIdentifier, CFBundleExecutable, …
  Name.app/Contents/MacOS/<Startdatei>  hier ein Shell-Skript, das das eigentliche Programm startet
  Name.app/Contents/Resources/AppIcon.svg

Systemprogramme liegen wie bei macOS in /System/Applications (Dienstprogramme in Utilities/),
Fundus wie der Finder in /System/Library/CoreServices, nachinstallierte Programme in /Applications.
Dazu kommt die Grundeinstellung des Docks in /Library/Preferences/com.clokeos.dock.plist.

  bundles.py <stage> <icons-ordner> <version> <build>

Zwei Schlüssel kennt nur CloakServer: CLWindowClasses (WM_CLASS der Fenster, * = beliebiger Teil),
damit Fenster zum Programm im Dock finden, und CLInternal für Programme, die CloakServer selbst
zeichnet (das Launchpad „Programme“).
"""
import colorsys
import os
import sys
from xml.sax.saxutils import escape


def term(ident, title_en, title_de, size, cmd):
    """Programm, das im Terminal läuft; die Fensterrolle ist die Bundle-Kennung. Der Fenstertitel
    folgt der Sprache von ClokeOS (/etc/clokeos-language)."""
    return (f"title='{title_en}'; [ \"$(cat /etc/clokeos-language 2>/dev/null)\" = de ] && title='{title_de}'\n"
            f"exec xfce4-terminal --disable-server --role={ident} --title=\"$title\" "
            f"--dynamic-title-mode=none --geometry={size} -x {cmd}")


# Search ist WebKit (clokesearch, webkit/README.md), wenn /opt/webkit installiert ist, sonst NetSurf mit der Startseite aus /share/clokeos (nur beim ersten Start, danach gilt, was NetSurf in
# Choices speichert; die mitgelieferte Willkommensseite von NetSurf wird ersetzt)
SEARCH_START = (
    "l=$(cut -c1-2 /etc/clokeos-language 2>/dev/null); case \"$l\" in de|fr|es|it) ;; *) l=en;; esac; t=hell; [ \"$CLOKEOS_DARK\" = 1 ] && t=dunkel\n"
    "start=/share/clokeos/search-$l-$t.html\n"
    "c=\"$HOME/.netsurf/Choices\"; mkdir -p \"$HOME/.netsurf\"\n"
    "{ grep -v -e '^homepage_url:' -e '^toolbar_items:' -e '^http_proxy' \"$c\" 2>/dev/null; echo \"homepage_url:file://$start\"; "
    "echo 'toolbar_items:back/history/forward/home/url_bar/reloadstop/openmenu'; } > \"$c.new\" && mv \"$c.new\" \"$c\"\n"
    "if [ -x /opt/webkit/bin/clokesearch ]; then BROWSER_BIN=clokesearch exec /opt/webkit/runmb.sh \"${1:-file://$start}\"; fi\n"
    "exec netsurf-gtk3 \"$@\"")

# Ordner, Name (englisch, deutsch), Kennung, Icon aus clokeos.html (oder None), Startzeile,
# Fensterklassen, intern. Wie bei macOS heißt das Paket englisch; den deutschen Namen findet
# CloakServer in Contents/Resources/de.lproj/InfoPlist.strings.
APPS = [
    ('System/Library/CoreServices', 'Fundus', 'Fundus', 'com.clokeos.fundus', 'dateien', 'exec thunar "$@"', 'Thunar', ''),
    ('System/Applications', 'Launchpad', 'Programme', 'com.clokeos.launchpad', 'programme', None, '', 'launchpad'),
    ('System/Applications', 'Search', 'Search', 'com.clokeos.search', 'search', SEARCH_START,
     'Netsurf-gtk3 Netsurf* NetSurf* ClokeSearch clokesearch', ''),
    ('System/Applications/Utilities', 'Terminal', 'Terminal', 'com.clokeos.terminal', 'terminal',
     'exec xfce4-terminal "$@"', 'Xfce4-terminal XTerm UXTerm', ''),
    ('System/Applications/Utilities', 'Linux', 'Linux', 'com.clokeos.linux', None, term('com.clokeos.linux', 'Linux', 'Linux', '80x24', 'tux-run'), '', ''),
    ('System/Applications', 'Notes', 'Notizen', 'com.clokeos.notizen', 'notizen',
     'exec /System/Library/CoreServices/ClokeApps/clokeapps notes', '', ''),
    ('System/Applications', 'Calculator', 'Rechner', 'com.clokeos.rechner', 'rechner', 'exec /System/Library/CoreServices/ClokeApps/clokeapps calc', '', ''),
    ('System/Applications', 'Calendar', 'Kalender', 'com.clokeos.kalender', 'kalender',
     'exec /System/Library/CoreServices/ClokeApps/clokeapps calendar', '', ''),
    ('System/Applications/Utilities', 'Activity Monitor', 'Aktivitätsanzeige', 'com.clokeos.aktivitaet', 'aktivitaet',
     'exec /System/Library/CoreServices/ClokeApps/clokeapps activity', '', ''),
    ('System/Applications/Utilities', 'Disk Utility', 'Festplattendienstprogramm', 'com.clokeos.festplatte', 'hd',
     None, '', 'diskutil'),
    ('System/Applications', 'System Settings', 'Systemeinstellungen', 'com.clokeos.systemeinstellungen', 'einstellungen',
     None, '', 'settings'),
    # Windows-Programme (Wine, nur x86_64): Dateimanager von Wine mit C: = /NT; .exe öffnet Fundus direkt
    ('Applications', 'Wine', 'Wine', 'org.winehq.wine', 'wine', 'exec wine winefile "$@"', 'winefile* wine* explorer* notepad* *.exe', ''),
    # nachinstalliert wie ein Programm aus dem Netz: liegt in /Applications
    ('Applications', 'Dillo', 'Dillo', 'org.dillo.dillo', None, 'exec dillo "$@"', 'Dillo', ''),
]

DOCK = ['com.clokeos.fundus', 'com.clokeos.launchpad', 'com.clokeos.search', 'com.clokeos.terminal',
        'com.clokeos.notizen', 'com.clokeos.rechner', 'com.clokeos.kalender', 'com.clokeos.aktivitaet',
        'com.clokeos.systemeinstellungen']

HEAD = ('<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0">\n<dict>\n')


def plist(items):
    out = [HEAD]
    for k, v in items:
        out.append(f'\t<key>{escape(k)}</key>\n')
        if isinstance(v, list):
            out.append('\t<array>\n' + ''.join(f'\t\t<string>{escape(s)}</string>\n' for s in v) + '\t</array>\n')
        else:
            out.append(f'\t<string>{escape(v)}</string>\n')
    out.append('</dict>\n</plist>\n')
    return ''.join(out)


def hsl(h, s, l):
    """librsvg 2.40 im Image kennt keine hsl()-Farben"""
    r, g, b = colorsys.hls_to_rgb(h / 360, l / 100, s / 100)
    return '#%02x%02x%02x' % (round(r * 255), round(g * 255), round(b * 255))


def generic_icon(name, hue):
    """Icon für Programme ohne eigenes: abgerundetes Quadrat mit Anfangsbuchstaben."""
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="{hsl(hue, 70, 62)}"/><stop offset="1" stop-color="{hsl(hue, 65, 42)}"/></linearGradient></defs>
<rect x="4" y="4" width="56" height="56" rx="13" fill="url(#g)"/>
<rect x="4.5" y="4.5" width="55" height="55" rx="12.5" fill="none" stroke="#fff" stroke-opacity=".25"/>
<text x="32" y="43" font-family="DejaVu Sans" font-weight="bold" font-size="30" fill="#fff" text-anchor="middle">{escape(name[0])}</text>
</svg>
'''


def linux_icon():
    """Icon für Linux.app: der Pinguin des Nutzers, liegt als tux/linux-icon.svg im Repository."""
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tux', 'linux-icon.svg'), encoding='utf-8') as f:
        return f.read()


def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
    os.chmod(path, mode)


def ascii_name(name):
    """Ordner- und Dateinamen ohne Leerzeichen und Umlaute (Activity Monitor -> ActivityMonitor): Der
    Anzeigename steht in der Info.plist, und die debugfs-Befehle von brand.sh trennen an Leerzeichen."""
    name = name.replace(' ', '')
    assert name.isascii(), name
    return name


def main(stage, icons, version, build):
    for folder, name, name_de, ident, icon, start, classes, internal in APPS:
        exe = ascii_name(name)
        app = os.path.join(stage, folder, exe + '.app', 'Contents')
        items = [('CFBundleDevelopmentRegion', 'en'), ('CFBundleDisplayName', name), ('CFBundleExecutable', exe),
                 ('CFBundleIconFile', 'AppIcon'), ('CFBundleIdentifier', ident),
                 ('CFBundleInfoDictionaryVersion', '6.0'), ('CFBundleName', name), ('CFBundlePackageType', 'APPL'),
                 ('CFBundleShortVersionString', version), ('CFBundleVersion', build)]
        if classes:
            items.append(('CLWindowClasses', classes))
        if internal:
            items.append(('CLInternal', internal))
        write(os.path.join(app, 'Info.plist'), plist(items))
        write(os.path.join(app, 'PkgInfo'), 'APPL????')
        if name_de != name:
            write(os.path.join(app, 'Resources', 'de.lproj', 'InfoPlist.strings'),
                  f'"CFBundleDisplayName" = "{name_de}";\n"CFBundleName" = "{name_de}";\n')
        script = '#!/bin/sh\n'
        script += start + '\n' if start else f'# {name} zeichnet CloakServer selbst ({internal}).\nexit 0\n'
        write(os.path.join(app, 'MacOS', exe), script, 0o755)
        if icon:
            own = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icons', icon + '.svg')
            with open(own if os.path.exists(own) else os.path.join(icons, icon + '.svg'), encoding='utf-8') as f:
                svg = f.read()
        elif name == 'Linux':
            svg = linux_icon()
        else:
            svg = generic_icon(name, sum(map(ord, name)) * 37 % 360)
        write(os.path.join(app, 'Resources', 'AppIcon.svg'), svg)
    write(os.path.join(stage, 'Library/Preferences/com.clokeos.dock.plist'), plist([('persistent-apps', DOCK)]))
    print('Bundles: %d Programme, Dock mit %d' % (len(APPS), len(DOCK)))


if __name__ == '__main__':
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
