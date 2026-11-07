#!/usr/bin/env python3
"""Ende-zu-Ende-Test der Oberfläche mit laufendem Dienst (Playwright + Chrome/Chromium).
Aufruf: python3 tests/test_ui.py [Pfad-zu-Chrome]"""
import hashlib, http.server, io, json, os, shutil, subprocess, sys, tarfile, tempfile, threading, time, re, signal
from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HELPER = os.path.join(ROOT, '..', 'overlay-desktop', 'System', 'Applications', 'ClokeStore.app', 'Contents', 'Resources', 'clokestore.py')
CHROME = sys.argv[1] if len(sys.argv) > 1 else shutil.which('google-chrome') or shutil.which('chromium')
PORT, SRC = 8771, 9111
ok = fail = 0
def check(n, c, x=''):
    global ok, fail
    ok += c; fail += (not c)
    print(('  ok   ' if c else '  FAIL ') + n + ('' if c else '  -> ' + str(x)))

def tgz(ver):
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode='w:gz') as t:
        for n, d, dir_ in (('Demo.app', b'', 1), ('Demo.app/Contents', b'', 1), ('Demo.app/Contents/Info.plist', b'<plist/>', 0), ('Demo.app/Contents/MacOS', b'', 1), ('Demo.app/Contents/MacOS/Demo', ('v' + ver).encode(), 0)):
            ti = tarfile.TarInfo(n); ti.mode = 0o755
            if dir_: ti.type = tarfile.DIRTYPE; t.addfile(ti)
            else: ti.size = len(d); t.addfile(ti, io.BytesIO(d))
    return b.getvalue()

tmp = tempfile.mkdtemp(prefix='csui-'); srcdir = os.path.join(tmp, 'src'); os.makedirs(srcdir)
pk = {v: tgz(v) for v in ('1', '2')}
for v, b in pk.items(): open(os.path.join(srcdir, 'demo-%s.tgz' % v), 'wb').write(b)
def write_index(ver):
    json.dump({'format': 1, 'repo': 'demo/Demo-Apps', 'version': ver, 'apps': [{
        'id': 'org.demo.app', 'name': 'Demo Notes', 'name_de': 'Demo-Notizen', 'path': '/Applications/Demo.app', 'category': 'addon', 'version': ver,
        'summary': 'A tiny demo app', 'summary_de': 'Eine kleine Demo-App', 'developer': 'Demo Co',
        'description_de': 'Zeigt, wie eine eigene Quelle aussieht.\n\nZweiter Absatz.', 'icon': 'http://127.0.0.1:%d/missing.png' % SRC,
        'package': {'url': 'http://127.0.0.1:%d/demo-%s.tgz' % (SRC, ver), 'sha256': hashlib.sha256(pk[ver]).hexdigest(), 'size': len(pk[ver])}}]},
        open(os.path.join(srcdir, 'index.json'), 'w'))
write_index('1')
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(s, *a, **k): super().__init__(*a, directory=srcdir, **k)
    def end_headers(s): s.send_header('Access-Control-Allow-Origin', '*'); super().end_headers()
    def log_message(s, *a): pass
srv = http.server.ThreadingHTTPServer(('127.0.0.1', SRC), H); threading.Thread(target=srv.serve_forever, daemon=True).start()
bindir = os.path.join(tmp, 'bin'); os.makedirs(bindir)
open(os.path.join(bindir, 'cloke-app'), 'w').write('#!/bin/sh\nsleep 1\nexit 0\n'); os.chmod(os.path.join(bindir, 'cloke-app'), 0o755)
apps = os.path.join(tmp, 'apps')
proc = subprocess.Popen([sys.executable, HELPER, '--no-open', '--keep-alive', '--port', str(PORT), '--state-dir', os.path.join(tmp, 'state'), '--apps-dir', apps],
                        stdout=subprocess.PIPE, text=True, env=dict(os.environ, PATH=bindir + os.pathsep + os.environ['PATH']))
base = re.search(r'(http://\S+)', proc.stdout.readline()).group(1)
errors = []
try:
    with sync_playwright() as p:
        b = p.chromium.launch(executable_path=CHROME, args=['--no-sandbox'])
        pg = b.new_context(viewport={'width': 1280, 'height': 860}, color_scheme='light', locale='de-DE').new_page()
        pg.on('pageerror', lambda e: errors.append(str(e)))
        pg.on('console', lambda m: errors.append(m.text) if m.type == 'error' and 'ERR_TUNNEL' not in m.text and 'Failed to load resource' not in m.text else None)
        pg.goto(base); pg.wait_for_selector('.hero')
        check('mit Dienst verbunden', 'Mit ClokeOS verbunden' in pg.inner_text('#conn'), pg.inner_text('#conn'))
        check('CSP blockiert nichts Eigenes (Seite rendert)', pg.locator('.hero').count() >= 1)

        print('Quelle hinzufügen')
        pg.click('a[href="#/sources"]'); pg.click('[data-act=add-source]')
        pg.fill('#src-in', 'ftp://example.com/x'); pg.click('#src-check')
        pg.wait_for_selector('.msg.err'); check('ftp-Adresse wird abgelehnt', 'https' in pg.inner_text('.msg.err'), pg.inner_text('.msg.err'))
        pg.fill('#src-in', 'http://127.0.0.1:%d/index.json' % SRC); pg.click('#src-check')
        pg.wait_for_selector('.found'); check('Quelle wird erkannt', 'demo/Demo-Apps' in pg.inner_text('.found') and '1 Programm' in pg.inner_text('.found'), pg.inner_text('.found'))
        pg.screenshot(path=os.environ.get('SHOT_DIR', tmp) + '/u1_addsource.png')
        pg.click('#src-add'); pg.wait_for_selector('.src >> nth=1')
        check('Quelle steht in der Liste', 'demo/Demo-Apps' in pg.inner_text('main'))

        print('Installieren')
        pg.fill('#q', 'demo'); pg.wait_for_selector('.row')
        check('Suche findet das Programm der neuen Quelle', 'Demo-Notizen' in pg.inner_text('main'))
        pg.click('.row .get'); pg.wait_for_selector('.get.working', timeout=3000)
        check('Fortschrittsring erscheint', pg.locator('.get.working .ring').count() == 1)
        pg.wait_for_selector('.row .get.installed', timeout=15000)
        check('Schaltfläche wechselt zu Installiert', 'Installiert' in pg.inner_text('.row .get'))
        check('Bundle liegt in ~/Applications', os.path.isfile(os.path.join(apps, 'Demo.app', 'Contents', 'MacOS', 'Demo')))

        print('Detailseite')
        pg.click('.row-main'); pg.wait_for_selector('.d-head')
        txt = pg.inner_text('main')
        check('Detailseite zeigt Entwickler, Quelle und Beschreibung', 'Demo Co' in txt and 'demo/Demo-Apps' in txt and 'Zweiter Absatz' in txt)
        check('Entfernen-Knopf sichtbar', pg.locator('[data-act=remove]').count() == 1)
        check('kaputtes Icon fällt auf Platzhalter zurück', pg.locator('.d-head .icon img').count() == 0 and pg.locator('.d-head .icon svg, .d-head .icon .ini').count() == 1)
        pg.screenshot(path=os.environ.get('SHOT_DIR', tmp) + '/u2_detail_ext.png')

        print('Update')
        write_index('2')
        pg.click('a[href="#/sources"]'); pg.click('[data-act=reload]')
        pg.wait_for_selector('#upbadge:not([hidden])', timeout=8000)
        check('Update-Zähler zeigt 1', pg.inner_text('#upbadge').strip() == '1', pg.inner_text('#upbadge'))
        pg.click('a[href="#/updates"]'); pg.wait_for_selector('.row .get.update')
        pg.screenshot(path=os.environ.get('SHOT_DIR', tmp) + '/u3_updates.png')
        pg.click('[data-act=update-all]'); pg.wait_for_selector('.empty', timeout=15000)
        check('nach „Alle aktualisieren“ ist alles aktuell', 'Alles aktuell' in pg.inner_text('main'), pg.inner_text('main')[:120])
        check('Bundle enthält jetzt Version 2', open(os.path.join(apps, 'Demo.app', 'Contents', 'MacOS', 'Demo')).read() == 'v2')

        print('Entfernen und Quelle löschen')
        pg.goto(base + '#/search/demo'); pg.wait_for_selector('.row'); pg.click('.row-main'); pg.wait_for_selector('[data-act=remove]')
        pg.click('[data-act=remove]'); pg.locator('[data-act=remove]').wait_for(state='detached', timeout=8000)
        check('Bundle ist weg', not os.path.exists(os.path.join(apps, 'Demo.app')))
        pg.click('a[href="#/sources"]'); pg.click('[data-act=remove-source]'); pg.locator('[data-act=remove-source]').wait_for(state='detached', timeout=8000)
        pg.wait_for_selector('.src', timeout=8000)                      # Liste wird nach dem Neuladen wieder aufgebaut
        check('Quelle entfernt, Standardquelle bleibt', 'ClokeOS-Apps' in pg.inner_text('main') and 'demo/Demo-Apps' not in pg.inner_text('main'), pg.inner_text('main')[:200])
        pg.click('[data-act=toggle-source][data-id=default]')
        pg.wait_for_selector('[data-act=toggle-source][data-id=default][aria-checked=false]', timeout=8000)
        check('Standardquelle lässt sich ausschalten', True)
        pg.click('[data-act=toggle-source][data-id=default]')
        pg.wait_for_selector('[data-act=toggle-source][data-id=default][aria-checked=true]', timeout=8000)

        print('Englisch')
        pg.click('[data-lang=en]'); pg.goto(base + '#/discover'); pg.wait_for_selector('.hero')
        check('Sprache wechselt auf Englisch', 'Discover' in pg.inner_text('#nav-main') and 'Windows programs' in pg.inner_text('.hero'), pg.inner_text('.hero')[:80])
        check('keine Konsolenfehler', not errors, errors)
        b.close()
finally:
    proc.send_signal(signal.SIGTERM); srv.shutdown(); shutil.rmtree(tmp, ignore_errors=True)
print('\n%d bestanden, %d fehlgeschlagen' % (ok, fail)); sys.exit(1 if fail else 0)
