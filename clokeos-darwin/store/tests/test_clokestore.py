#!/usr/bin/env python3
"""Tests für clokestore.py: startet den Dienst, eine Fremdquelle und ein Stub-`cloke-app`.

Aufruf:  python3 tests/test_clokestore.py
"""
import hashlib, http.server, io, json, os, re, shutil, signal, stat, subprocess, sys, tarfile, tempfile, threading, time, urllib.error, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HELPER = os.path.join(ROOT, '..', 'overlay-desktop', 'System', 'Applications', 'ClokeStore.app', 'Contents', 'Resources', 'clokestore.py')
PORT, SRC_PORT = 8761, 9101
ok = fail = 0


def check(name, cond, extra=''):
    global ok, fail
    if cond:
        ok += 1
        print('  ok   ' + name)
    else:
        fail += 1
        print('  FAIL ' + name + ('  -> ' + str(extra) if extra else ''))


def make_tgz(files):
    """files: Liste (name, bytes|None für Ordner, mode, typ, linkname)"""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz') as tf:
        for name, data, mode, typ, link in files:
            ti = tarfile.TarInfo(name)
            ti.mode = mode
            if typ == 'dir':
                ti.type = tarfile.DIRTYPE
                tf.addfile(ti)
            elif typ == 'sym':
                ti.type = tarfile.SYMTYPE; ti.linkname = link
                tf.addfile(ti)
            elif typ == 'hard':
                ti.type = tarfile.LNKTYPE; ti.linkname = link
                tf.addfile(ti)
            else:
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def demo_app(version, name='Demo'):
    plist = ('<?xml version="1.0"?><plist version="1.0"><dict><key>CFBundleName</key><string>%s</string></dict></plist>' % name).encode()
    return make_tgz([
        (name + '.app', None, 0o755, 'dir', None),
        (name + '.app/Contents', None, 0o755, 'dir', None),
        (name + '.app/Contents/Info.plist', plist, 0o644, 'file', None),
        (name + '.app/Contents/MacOS', None, 0o755, 'dir', None),
        (name + '.app/Contents/MacOS/' + name, ('#!/bin/sh\necho v%s\n' % version).encode(), 0o4755, 'file', None),   # setuid-Bit muss verschwinden
    ])


def api(base, token, path, body=None, headers=None, method=None):
    h = {'X-ClokeStore-Token': token, 'Content-Type': 'application/json'}
    h.update(headers or {})
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data, headers=h, method=method or ('POST' if data is not None else 'GET'))
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'{}')


def wait_job(base, token, job):
    for _ in range(100):
        s, j = api(base, token, '/api/jobs/' + job)
        if j.get('state') != 'running':
            return j
        time.sleep(0.1)
    return {'state': 'timeout'}


def main():
    tmp = tempfile.mkdtemp(prefix='cstest-')
    srcdir, bindir, state, apps = (os.path.join(tmp, d) for d in ('src', 'bin', 'state', 'apps'))
    for d in (srcdir, bindir):
        os.makedirs(d)
    pkgs = {
        'demo-1.tgz': demo_app('1'),
        'demo-2.tgz': demo_app('2'),
        'evil-dotdot.tgz': make_tgz([('../evil.txt', b'x', 0o644, 'file', None)]),
        'evil-abs.tgz': make_tgz([('/tmp/cs-evil-abs', b'x', 0o644, 'file', None)]),
        'evil-sym.tgz': make_tgz([('Bad.app', None, 0o755, 'dir', None), ('Bad.app/link', None, 0o777, 'sym', '../../../../etc')]),
        'evil-hard.tgz': make_tgz([('Bad.app', None, 0o755, 'dir', None), ('Bad.app/h', None, 0o644, 'hard', '/etc/passwd')]),
        'noapp.tgz': make_tgz([('readme.txt', b'hi', 0o644, 'file', None)]),
    }
    for n, b in pkgs.items():
        with open(os.path.join(srcdir, n), 'wb') as f:
            f.write(b)
    sha = lambda n: hashlib.sha256(pkgs[n]).hexdigest()
    pk = lambda n, s=None: {'url': 'http://127.0.0.1:%d/%s' % (SRC_PORT, n), 'sha256': s or sha(n), 'size': len(pkgs[n])}

    # Stub-cloke-app
    stub = os.path.join(bindir, 'cloke-app')
    with open(stub, 'w') as f:
        f.write('#!/bin/sh\necho "$@" >> "%s/calls.txt"\nif [ "$2" = "fail.app" ]; then echo "kein Netz"; exit 3; fi\necho "installiere $2"\nexit 0\n' % tmp)
    os.chmod(stub, 0o755)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=srcdir, **k)
        def log_message(self, *a):
            pass
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', SRC_PORT), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    env = dict(os.environ, PATH=bindir + os.pathsep + os.environ['PATH'])
    proc = subprocess.Popen([sys.executable, HELPER, '--no-open', '--keep-alive', '--port', str(PORT), '--state-dir', state, '--apps-dir', apps],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    try:
        line = proc.stdout.readline()
        m = re.search(r'http://127\.0\.0\.1:(\d+)/', line)
        assert m, line
        base = 'http://127.0.0.1:%s' % m.group(1)
        print('Dienst:', base)

        print('Seite und Zugriffsschutz')
        with urllib.request.urlopen(base + '/') as r:
            page = r.read().decode()
            csp = r.headers.get('Content-Security-Policy', '')
        tm = re.search(r'name="clokestore-token" content="([^"]+)"', page)
        check('Seite enthält Token', bool(tm))
        token = tm.group(1)
        check('CSP mit Skript-Hash, ohne unsafe-inline für Skripte', "script-src 'sha256-" in csp and "script-src 'unsafe-inline'" not in csp, csp)
        req = urllib.request.Request(base + '/', headers={'Host': 'evil.example'})
        try:
            urllib.request.urlopen(req); code = 200
        except urllib.error.HTTPError as e:
            code = e.code
        check('fremder Host-Header wird abgelehnt (DNS-Rebinding)', code == 403, code)
        s, _ = api(base, 'falsch', '/api/state', {'items': []})
        check('falsches Token wird abgelehnt', s == 403, s)
        s, _ = api(base, token, '/api/state', {'items': []}, headers={'Origin': 'http://evil.example'})
        check('fremde Herkunft wird abgelehnt', s == 403, s)
        s, _ = api(base, token, '/api/state', {'items': []}, headers={'Content-Type': 'text/plain'})
        check('kein JSON-Inhaltstyp wird abgelehnt', s == 415, s)
        s, j = api(base, token, '/api/state', {'items': []})
        check('Zustand mit Token', s == 200 and j['arch'] in ('arm64', 'x86_64') and j['cloke_app'] is True, j)

        print('Eigene Quelle')
        req_demo = lambda n, ver, **kw: dict({'key': 'ext:demo', 'source': 'external', 'id': 'demo.app', 'kind': 'app', 'name': 'Demo', 'version': ver, 'path': '/Applications/Demo.app', 'category': 'addon', 'pkg': pk(n)}, **kw)
        s, j = api(base, token, '/api/install', req_demo('demo-1.tgz', '1'))
        done = wait_job(base, token, j['job']) if s == 200 else {}
        check('Installation läuft durch', done.get('state') == 'done', done)
        exe = os.path.join(apps, 'Demo.app', 'Contents', 'MacOS', 'Demo')
        check('Bundle liegt in ~/Applications', os.path.isfile(exe))
        mode = os.stat(exe).st_mode
        check('Ausführbit bleibt, setuid wird entfernt', bool(mode & stat.S_IXUSR) and not (mode & stat.S_ISUID), oct(mode))
        s, j = api(base, token, '/api/state', {'items': [{'key': 'ext:demo', 'id': 'demo.app', 'kind': 'app', 'path': '/Applications/Demo.app', 'builtin': False}]})
        check('Zustand: installiert, Version 1, verwaltet', j['installed'].get('ext:demo', {}).get('version') == '1' and j['installed']['ext:demo']['managed'], j)
        s, j = api(base, token, '/api/install', req_demo('demo-2.tgz', '2'))
        done = wait_job(base, token, j['job'])
        with open(exe) as f:
            body = f.read()
        check('Aktualisierung ersetzt das Bundle', done.get('state') == 'done' and 'v2' in body, (done, body))
        check('keine Reste im Zielordner', sorted(os.listdir(apps)) == ['Demo.app'], os.listdir(apps))

        s, j = api(base, token, '/api/install', req_demo('demo-1.tgz', '1', pkg=pk('demo-1.tgz', 'a' * 64)))
        done = wait_job(base, token, j['job'])
        check('falsche Prüfsumme wird verworfen', done.get('state') == 'error' and 'Prüfsumme' in done.get('message', ''), done)
        for n, why in (('evil-dotdot.tgz', 'Pfad mit ..'), ('evil-abs.tgz', 'absoluter Pfad'), ('evil-sym.tgz', 'Symlink nach außen'), ('evil-hard.tgz', 'Hardlink'), ('noapp.tgz', 'kein Bundle')):
            s, j = api(base, token, '/api/install', req_demo(n, '1', key='ext:bad', id='bad.app', path='/Applications/Bad.app'))
            done = wait_job(base, token, j['job'])
            check('Archiv abgelehnt: ' + why, done.get('state') == 'error', done)
        check('Fremdes Archiv hat nichts außerhalb geschrieben', not os.path.exists('/tmp/cs-evil-abs') and not os.path.exists(os.path.join(tmp, 'evil.txt')))
        s, j = api(base, token, '/api/install', req_demo('demo-1.tgz', '1', category='system'))
        check('Systemprogramm aus Fremdquelle abgelehnt', s == 400, (s, j))
        s, j = api(base, token, '/api/install', req_demo('demo-1.tgz', '1', kind='package'))
        check('Paket aus Fremdquelle abgelehnt', s == 400, (s, j))
        s, j = api(base, token, '/api/install', req_demo('demo-1.tgz', '1', pkg={'url': 'http://example.com/x.tgz', 'sha256': 'a' * 64}))
        done = wait_job(base, token, j['job']) if s == 200 else {'state': 'error', 'message': str(j)}
        check('http-Adresse (nicht lokal) abgelehnt', done.get('state') == 'error' and 'https' in done.get('message', ''), done)
        os.makedirs(os.path.join(apps, 'Mine.app', 'Contents'))
        open(os.path.join(apps, 'Mine.app', 'Contents', 'Info.plist'), 'w').write('x')
        with open(os.path.join(srcdir, 'mine.tgz'), 'wb') as f:
            f.write(demo_app('1', 'Mine'))
        pkgs['mine.tgz'] = demo_app('1', 'Mine')
        s, j = api(base, token, '/api/install', req_demo('mine.tgz', '1', key='ext:mine', id='mine.app', path='/Applications/Mine.app'))
        done = wait_job(base, token, j['job'])
        check('vorhandenes fremdes Bundle wird nicht überschrieben', done.get('state') == 'error' and 'nicht aus dem Store' in done.get('message', ''), done)

        print('Standardquelle (cloke-app)')
        base_req = {'key': 'default:org.dillo.dillo', 'source': 'default', 'id': 'org.dillo.dillo', 'kind': 'app', 'version': '2.8', 'path': '/Applications/Dillo.app', 'category': 'addon'}
        s, j = api(base, token, '/api/install', base_req)
        done = wait_job(base, token, j['job'])
        calls = open(os.path.join(tmp, 'calls.txt')).read().split('\n')
        check('ruft cloke-app install <kennung> auf', done.get('state') == 'done' and 'install org.dillo.dillo' in calls, (done, calls))
        s, j = api(base, token, '/api/install', dict(base_req, key='default:webkit', id='webkit', kind='package', path=''))
        check('Paket aus Standardquelle erlaubt', s == 200 and wait_job(base, token, j['job']).get('state') == 'done')
        s, j = api(base, token, '/api/install', dict(base_req, key='default:fail.app', id='fail.app'))
        done = wait_job(base, token, j['job'])
        check('Fehler von cloke-app erscheint im Auftrag', done.get('state') == 'error' and 'kein Netz' in done.get('message', ''), done)
        s, j = api(base, token, '/api/install', dict(base_req, id='--help'))
        check('Kennung mit führendem Bindestrich abgelehnt', s == 400, (s, j))
        s, j = api(base, token, '/api/install', dict(base_req, id='a b;rm -rf'))
        check('Kennung mit Sonderzeichen abgelehnt', s == 400, (s, j))

        print('Zustand und Entfernen')
        sysapp = os.path.join(tmp, 'Sys.app'); os.makedirs(sysapp)
        s, j = api(base, token, '/api/state', {'items': [{'key': 'default:sys', 'id': 'sys', 'kind': 'app', 'path': sysapp, 'builtin': True}, {'key': 'ext:nope', 'id': 'nope', 'kind': 'app', 'path': sysapp, 'builtin': False}]})
        check('vorhandenes Systemprogramm gilt als installiert (ohne Version)', j['installed'].get('default:sys', {}).get('version') == '' and 'ext:nope' not in j['installed'], j)
        s, j = api(base, token, '/api/remove', {'key': 'default:org.dillo.dillo'})
        check('nicht verwaltetes Programm lässt sich nicht entfernen', s == 400, (s, j))
        s, j = api(base, token, '/api/remove', {'key': 'ext:demo'})
        check('verwaltetes Programm wird entfernt', s == 200 and not os.path.exists(os.path.join(apps, 'Demo.app')), (s, j))
        rec = json.load(open(os.path.join(state, 'installed.json')))
        check('Eintrag ist aus dem Protokoll verschwunden', 'ext:demo' not in rec and 'default:org.dillo.dillo' in rec, list(rec))
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        srv.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    print('\n%d bestanden, %d fehlgeschlagen' % (ok, fail))
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
