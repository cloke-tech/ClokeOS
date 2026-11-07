#!/usr/bin/env python3
"""Cloke Store: lokaler Dienst für den App Store von ClokeOS.

Er macht zwei Dinge:
  1. liefert die Oberfläche (index.html) unter http://127.0.0.1:<port>/ aus,
  2. führt Installationen aus, die die Oberfläche anfordert.

Standardquelle (cloke-tech/ClokeOS-Apps): der Dienst ruft `cloke-app install <kennung>` auf.
Eigene Quellen: Paket laden, SHA-256 prüfen, sicher entpacken, das .app-Bundle nach
~/Applications legen. Systemprogramme und Pakete kommen nur aus der Standardquelle.

Nur Standardbibliothek, Python 3.8 oder neuer.
"""
import argparse
import base64
import hashlib
import hmac
import http.server
import json
import os
import platform
import posixpath
import re
import secrets
import shutil
import socketserver
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.parse

# Pythons urllib.request lädt unter macOS _scproxy; auf ClokeOS gibt es das nicht (siehe cloke).
if sys.platform == 'darwin':
    try:
        import _scproxy  # noqa: F401
    except ImportError:
        import types
        _m = types.ModuleType('_scproxy')
        _m._get_proxy_settings = lambda: {'exclude_simple': False, 'exceptions': []}
        _m._get_proxies = lambda: {}
        sys.modules['_scproxy'] = _m
import urllib.request  # noqa: E402
import webbrowser

try:
    import ssl  # noqa: F401
    HAVE_SSL = True
except ImportError:
    HAVE_SSL = False

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_DOWNLOAD = 400 * 1024 * 1024
MAX_UNPACKED = 1024 * 1024 * 1024
IDLE_SECONDS = 300          # so lange darf die Oberfläche still sein, bevor der Dienst endet
START_GRACE = 600           # so lange wartet er auf das erste Öffnen

ID_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')
SHA_RE = re.compile(r'^[0-9a-f]{64}$')
BUNDLE_RE = re.compile(r'^[\w][\w .+-]{0,80}\.app$')


class Fail(Exception):
    """Fehler mit Text, der in der Oberfläche erscheint."""


def machine_arch():
    m = platform.machine().lower()
    if m in ('arm64', 'aarch64'):
        return 'arm64'
    if m in ('x86_64', 'amd64'):
        return 'x86_64'
    return m


def http_ok(url):
    try:
        u = urllib.parse.urlparse(url)
    except ValueError:
        return False
    if u.scheme == 'https' and u.hostname:
        return True
    return u.scheme == 'http' and u.hostname in ('127.0.0.1', 'localhost')


class Store:
    def __init__(self, state_dir, apps_dir):
        self.state_dir = state_dir
        self.apps_dir = apps_dir
        self.records_path = os.path.join(state_dir, 'installed.json')
        self.lock = threading.Lock()
        self.jobs = {}
        self.busy = set()
        os.makedirs(state_dir, exist_ok=True)

    # ---- Protokoll der installierten Einträge ----
    def records(self):
        try:
            with open(self.records_path, encoding='utf-8') as f:
                data = json.load(f)
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def save_record(self, key, rec=None):
        with self.lock:
            data = self.records()
            if rec is None:
                data.pop(key, None)
            else:
                data[key] = rec
            tmp = self.records_path + '.tmp'
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=1, sort_keys=True)
            os.replace(tmp, self.records_path)

    def state(self, items):
        recs = self.records()
        installed = {}
        for it in items:
            key = it.get('key')
            if not isinstance(key, str):
                continue
            rec = recs.get(key)
            if rec and rec.get('managed') and not os.path.isdir(rec.get('path', '')):
                rec = None                                   # Bundle wurde von Hand gelöscht
            if rec:
                installed[key] = {'version': rec.get('version', ''), 'path': rec.get('path', ''), 'managed': bool(rec.get('managed'))}
            elif it.get('builtin') and it.get('kind') == 'app':
                p = it.get('path') or ''
                if p.startswith('/') and p.endswith('.app') and '..' not in p.split('/') and os.path.isdir(p):
                    installed[key] = {'version': '', 'path': p, 'managed': False}
        return {'arch': machine_arch(), 'cloke_app': bool(shutil.which('cloke') or shutil.which('cloke-app')), 'installed': installed}

    # ---- Aufträge ----
    def start_install(self, req):
        key = req.get('key')
        if not isinstance(key, str) or len(key) > 200:
            raise Fail('Ungültige Anfrage')
        if not ID_RE.match(str(req.get('id', ''))):
            raise Fail('Ungültige Kennung')
        source = req.get('source')
        if source not in ('default', 'external'):
            raise Fail('Ungültige Quelle')
        if key in self.busy:
            raise Fail('Für dieses Programm läuft schon eine Installation')
        if source == 'external':
            if req.get('kind') != 'app':
                raise Fail('Pakete gibt es nur aus der Standardquelle')
            if req.get('category') == 'system':
                raise Fail('Systemprogramme gibt es nur aus der Standardquelle')
        job_id = secrets.token_hex(8)
        job = {'state': 'running', 'pct': None, 'message': ''}
        self.jobs[job_id] = job
        self.busy.add(key)
        threading.Thread(target=self._run, args=(job, req), daemon=True).start()
        if len(self.jobs) > 50:                               # alte Aufträge verwerfen
            for k in [k for k, v in self.jobs.items() if v['state'] != 'running'][:-20]:
                self.jobs.pop(k, None)
        return job_id

    def _run(self, job, req):
        try:
            if req['source'] == 'default':
                self._install_default(job, req)
            else:
                self._install_external(job, req)
            job['pct'] = 100
            job['state'] = 'done'
        except Fail as e:
            job['state'], job['message'] = 'error', str(e)
        except Exception as e:  # unerwartet: mit Typ melden, damit man es findet
            job['state'], job['message'] = 'error', '%s: %s' % (type(e).__name__, e)
        finally:
            self.busy.discard(req['key'])

    def _install_default(self, job, req):
        exe = shutil.which('cloke')
        if exe:
            # cloke 0.2: Programme für den Benutzer; Systemprogramme und Pakete systemweit (braucht root oder sudo)
            cmd = [exe, 'install', '-y', '--from', 'apps']
            if req.get('kind') != 'app' or req.get('category') == 'system':
                cmd.append('--system')
            cmd.append(req['id'])
        elif shutil.which('cloke-app'):
            cmd = [shutil.which('cloke-app'), 'install', req['id']]
        else:
            raise Fail('cloke wurde nicht gefunden (ClokeOS: /usr/bin/cloke)')
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        tail = []
        for line in proc.stdout:
            tail = (tail + [line.rstrip()])[-6:]
        if proc.wait() != 0:
            raise Fail(' '.join(x for x in tail if x)[:300] or 'cloke ist mit einem Fehler beendet')
        path, managed = str(req.get('path', ''))[:300], False
        if cmd[0] == exe and '--system' not in cmd:
            # cloke legt Programme ohne --system nach ~/Applications: dann gehören sie dem Store wie eigene
            path, managed = os.path.join(self.apps_dir, os.path.basename(path)), True
        self.save_record(req['key'], {'id': req['id'], 'version': str(req.get('version', ''))[:40], 'path': path, 'managed': managed, 'source': 'default', 'at': int(time.time())})

    def _pick_package(self, req):
        pkg = req.get('pkg')
        if not isinstance(pkg, dict):
            raise Fail('Das Paket fehlt in der Anfrage')
        url, sha = pkg.get('url'), str(pkg.get('sha256', '')).lower()
        if not isinstance(url, str) or not http_ok(url):
            raise Fail('Die Paketadresse ist nicht erlaubt (nur https)')
        if not SHA_RE.match(sha):
            raise Fail('Die Prüfsumme fehlt oder ist ungültig')
        return url, sha

    def _install_external(self, job, req):
        url, sha = self._pick_package(req)
        os.makedirs(self.apps_dir, exist_ok=True)
        work = tempfile.mkdtemp(prefix='clokestore-')
        try:
            archive = os.path.join(work, 'paket.tgz')
            self._download(job, url, sha, archive)
            unpacked = os.path.join(work, 'inhalt')
            os.makedirs(unpacked)
            job['pct'] = 82
            self._safe_extract(archive, unpacked)
            job['pct'] = 92
            bundle = self._find_bundle(unpacked, os.path.basename(str(req.get('path', ''))))
            self._place(bundle, req)
        finally:
            shutil.rmtree(work, ignore_errors=True)

    def _download(self, job, url, sha, dest):
        h = hashlib.sha256()
        total = 0
        if url.startswith('https:') and not HAVE_SSL and shutil.which('curl'):
            # Python auf ClokeOS hat kein ssl-Modul: https über curl
            p = subprocess.run(['curl', '-fsSL', '--max-time', '300', '--max-filesize', str(MAX_DOWNLOAD), '-A', 'ClokeStore/1', '-o', dest, url],
                               capture_output=True, text=True)
            if p.returncode != 0:
                raise Fail('Download fehlgeschlagen: %s' % ((p.stderr or '').strip() or 'curl %d' % p.returncode))
            with open(dest, 'rb') as f:
                for chunk in iter(lambda: f.read(65536), b''):
                    h.update(chunk)
            if not hmac.compare_digest(h.hexdigest(), sha):
                raise Fail('Die Prüfsumme stimmt nicht überein. Das Paket wurde verworfen.')
            return
        try:
            r = urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'ClokeStore/1'}), timeout=30)
        except Exception as e:
            raise Fail('Download fehlgeschlagen: %s' % getattr(e, 'reason', e))
        with r, open(dest, 'wb') as f:
            size = int(r.headers.get('Content-Length') or 0)
            if size > MAX_DOWNLOAD:
                raise Fail('Das Paket ist größer als erlaubt')
            while True:
                chunk = r.read(65536)
                if not chunk:
                    break
                total += len(chunk)
                if total > MAX_DOWNLOAD:
                    raise Fail('Das Paket ist größer als erlaubt')
                h.update(chunk)
                f.write(chunk)
                if size:
                    job['pct'] = min(80, int(total * 80 / size))
        if not hmac.compare_digest(h.hexdigest(), sha):
            raise Fail('Die Prüfsumme stimmt nicht überein. Das Paket wurde verworfen.')

    @staticmethod
    def _safe_extract(archive, dest):
        try:
            tf = tarfile.open(archive, 'r:*')
        except (tarfile.TarError, OSError) as e:
            raise Fail('Das Paket lässt sich nicht öffnen: %s' % e)
        with tf:
            members, total = [], 0
            for m in tf:
                parts = posixpath.normpath(m.name).split('/')
                if posixpath.isabs(m.name) or '..' in parts:
                    raise Fail('Das Paket enthält unzulässige Pfade')
                if not (m.isreg() or m.isdir() or m.issym()):
                    raise Fail('Das Paket enthält unzulässige Einträge (Links oder Geräte)')
                if m.issym():
                    target = posixpath.normpath(posixpath.join(posixpath.dirname(m.name), m.linkname))
                    if posixpath.isabs(m.linkname) or target == '..' or target.startswith('../'):
                        raise Fail('Das Paket enthält einen Link aus dem Ordner heraus')
                total += max(m.size, 0)
                if total > MAX_UNPACKED or len(members) > 50000:
                    raise Fail('Das entpackte Paket wäre zu groß')
                m.mode &= 0o755
                m.uid, m.gid, m.uname, m.gname = os.getuid(), os.getgid(), '', ''
                members.append(m)
            try:
                tf.extractall(dest, members=members, filter='data')
            except TypeError:                                  # Python ohne extraction filters
                tf.extractall(dest, members=members)
            except tarfile.TarError as e:
                raise Fail('Entpacken fehlgeschlagen: %s' % e)

    @staticmethod
    def _find_bundle(root, wanted):
        found = []
        for cur, dirs, _ in os.walk(root):
            depth = os.path.relpath(cur, root).count(os.sep)
            for d in list(dirs):
                if d.endswith('.app'):
                    found.append(os.path.join(cur, d))
                    dirs.remove(d)                              # nicht in Bundles hineinsuchen
            if depth >= 4:
                dirs[:] = []
        if wanted:
            named = [p for p in found if os.path.basename(p) == wanted]
            if named:
                found = named
        if len(found) != 1:
            raise Fail('Im Paket liegt kein eindeutiges .app-Bundle' if not found else 'Im Paket liegen mehrere .app-Bundles')
        if not os.path.isfile(os.path.join(found[0], 'Contents', 'Info.plist')):
            raise Fail('Das Bundle ist ungültig (Contents/Info.plist fehlt)')
        return found[0]

    def _place(self, bundle, req):
        name = os.path.basename(bundle)
        if not BUNDLE_RE.match(name):
            raise Fail('Der Bundle-Name ist nicht erlaubt')
        dest = os.path.join(self.apps_dir, name)
        rec = self.records().get(req['key'])
        if os.path.lexists(dest) and not (rec and rec.get('managed') and os.path.realpath(rec.get('path', '')) == os.path.realpath(dest)):
            raise Fail('%s liegt schon in ~/Applications und stammt nicht aus dem Store' % name)
        stage = tempfile.mkdtemp(prefix='.clokestore-stage-', dir=self.apps_dir)
        old = None
        try:
            staged = os.path.join(stage, name)
            shutil.move(bundle, staged)
            if os.path.lexists(dest):
                old = os.path.join(stage, 'alt-' + name)
                os.rename(dest, old)
            try:
                os.rename(staged, dest)
            except OSError:
                if old:
                    os.rename(old, dest)                         # zurückrollen
                raise
        finally:
            shutil.rmtree(stage, ignore_errors=True)
        self.save_record(req['key'], {'id': req['id'], 'version': str(req.get('version', ''))[:40], 'path': dest, 'managed': True, 'source': str(req.get('src', ''))[:300], 'at': int(time.time())})

    def remove(self, key):
        rec = self.records().get(key)
        if not rec or not rec.get('managed'):
            raise Fail('Dieses Programm hat der Store nicht installiert')
        path = os.path.realpath(rec['path'])
        root = os.path.realpath(self.apps_dir)
        if os.path.dirname(path) != root or not path.endswith('.app'):
            raise Fail('Der Pfad liegt außerhalb von ~/Applications')
        shutil.rmtree(path, ignore_errors=False)
        if rec.get('source') == 'default' and shutil.which('cloke'):
            # cloke führt ein eigenes Protokoll; Eintrag dort ebenfalls löschen (der Ordner ist schon weg)
            subprocess.run([shutil.which('cloke'), 'remove', str(rec.get('id', ''))], capture_output=True)
        self.save_record(key, None)


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = 'ClokeStore'
    sys_version = ''

    # Zugriff nur über 127.0.0.1 oder localhost, mit dem Token aus der ausgelieferten Seite
    def _hosts(self):
        port = self.server.server_address[1]
        return {'127.0.0.1:%d' % port, 'localhost:%d' % port}

    def _send(self, code, body, ctype='application/json; charset=utf-8', extra=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _guard(self, api=False):
        host = self.headers.get('Host', '')
        if host not in self._hosts():
            self._send(403, {'error': 'Host nicht erlaubt'})
            return False
        origin = self.headers.get('Origin')
        if origin and origin not in {'http://' + h for h in self._hosts()}:
            self._send(403, {'error': 'Herkunft nicht erlaubt'})
            return False
        if api:
            tok = self.headers.get('X-ClokeStore-Token', '')
            if not hmac.compare_digest(tok.encode(), self.server.token.encode()):
                self._send(403, {'error': 'Token fehlt oder ist falsch'})
                return False
        return True

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path in ('/', '/index.html'):
            if not self._guard():
                return
            body, csp = self.server.page
            self._send(200, body, 'text/html; charset=utf-8', {'Content-Security-Policy': csp})
        elif path.startswith('/api/jobs/'):
            if not self._guard(True):
                return
            job = self.server.store.jobs.get(path.rsplit('/', 1)[-1])
            if not job:
                return self._send(404, {'error': 'Auftrag unbekannt'})
            self._send(200, job)
        else:
            self._send(404, {'error': 'nicht gefunden'})

    def do_POST(self):
        path = urllib.parse.urlsplit(self.path).path
        if not path.startswith('/api/'):
            return self._send(404, {'error': 'nicht gefunden'})
        if not self._guard(True):
            return
        if 'application/json' not in self.headers.get('Content-Type', ''):
            return self._send(415, {'error': 'JSON erwartet'})
        try:
            n = int(self.headers.get('Content-Length') or 0)
            if n > 1024 * 1024:
                return self._send(413, {'error': 'Anfrage zu groß'})
            body = json.loads(self.rfile.read(n) or b'{}')
            if not isinstance(body, dict):
                raise ValueError
        except ValueError:
            return self._send(400, {'error': 'Ungültiges JSON'})
        store = self.server.store
        try:
            if path == '/api/ping':
                self.server.last_ping = time.time()
                self.server.seen_ping = True
                self._send(200, {'ok': True})
            elif path == '/api/state':
                self.server.last_ping = time.time()
                self.server.seen_ping = True
                items = body.get('items')
                self._send(200, store.state(items if isinstance(items, list) else []))
            elif path == '/api/install':
                self._send(200, {'job': store.start_install(body)})
            elif path == '/api/remove':
                store.remove(str(body.get('key', '')))
                self._send(200, {'ok': True})
            else:
                self._send(404, {'error': 'nicht gefunden'})
        except Fail as e:
            self._send(400, {'error': str(e)})
        except OSError as e:
            self._send(500, {'error': 'Dateifehler: %s' % e.strerror})

    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write('[clokestore] ' + (fmt % args) + '\n')


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def open_browser(url):
    """Browser öffnen. CLOKESTORE_BROWSER (Befehl, die Adresse wird angehängt) hat Vorrang; auf ClokeOS
    das WebKit-Search, sonst der Standardbrowser des Systems. Die Oberfläche braucht ein modernes
    JavaScript (fetch, Module); NetSurf reicht dafür nicht."""
    cmd = os.environ.get('CLOKESTORE_BROWSER')
    if cmd:
        subprocess.Popen(cmd.split() + [url])
    elif os.path.exists('/opt/webkit/runmb.sh'):
        env = dict(os.environ, BROWSER_BIN='clokesearch')
        subprocess.Popen(['/opt/webkit/runmb.sh', url], env=env)
    else:
        webbrowser.open(url)


def build_page(token):
    with open(os.path.join(HERE, 'index.html'), encoding='utf-8') as f:
        html = f.read()
    marker = '<meta name="clokestore-token" content="">'
    if marker not in html:
        raise SystemExit('index.html enthält den Token-Platzhalter nicht')
    html = html.replace(marker, '<meta name="clokestore-token" content="%s">' % token, 1)
    m = re.search(r'<script>(.*?)</script>', html, re.S)
    digest = base64.b64encode(hashlib.sha256(m.group(1).encode('utf-8')).digest()).decode()
    csp = ("default-src 'none'; script-src 'sha256-%s'; style-src 'unsafe-inline'; "
           "img-src 'self' data: https: http://127.0.0.1:* http://localhost:*; "
           "connect-src 'self' https: http://127.0.0.1:* http://localhost:*; "
           "base-uri 'none'; form-action 'none'; frame-ancestors 'none'") % digest
    return html.encode('utf-8'), csp


def main(argv=None):
    ap = argparse.ArgumentParser(description='Cloke Store: lokaler Dienst für den ClokeOS App Store')
    ap.add_argument('--port', type=int, default=8737)
    ap.add_argument('--no-open', action='store_true', help='Browser nicht automatisch öffnen')
    ap.add_argument('--keep-alive', action='store_true', help='auch weiterlaufen, wenn die Oberfläche geschlossen wurde')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--state-dir', default=os.path.expanduser('~/Library/Application Support/ClokeStore'))
    ap.add_argument('--apps-dir', default=os.path.expanduser('~/Applications'))
    args = ap.parse_args(argv)

    token = secrets.token_urlsafe(24)
    server = None
    for port in range(args.port, args.port + 11) if args.port else [0]:
        try:
            server = Server(('127.0.0.1', port), Handler)
            break
        except OSError:
            continue
    if server is None:
        raise SystemExit('Kein freier Port ab %d' % args.port)
    server.token = token
    server.store = Store(args.state_dir, args.apps_dir)
    server.page = build_page(token)
    server.verbose = args.verbose
    server.last_ping = time.time()
    server.seen_ping = False
    url = 'http://127.0.0.1:%d/' % server.server_address[1]
    print('Cloke Store läuft unter ' + url, flush=True)

    def watchdog():
        started = time.time()
        while True:
            time.sleep(5)
            idle = time.time() - server.last_ping
            if (server.seen_ping and idle > IDLE_SECONDS and not server.store.busy) or (not server.seen_ping and time.time() - started > START_GRACE):
                server.shutdown()
                return
    if not args.keep_alive:
        threading.Thread(target=watchdog, daemon=True).start()
    if not args.no_open:
        threading.Timer(0.4, lambda: open_browser(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
