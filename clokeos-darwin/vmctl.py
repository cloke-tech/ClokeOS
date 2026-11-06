#!/usr/bin/env python3
"""Steuert eine mit ./run.sh --headless gestartete ClokeOS-VM.

  ./vmctl.py wait '<regex>' [sekunden]   wartet, bis die Konsole den Text zeigt
  ./vmctl.py send '<befehl>' [sekunden]  tippt einen Befehl in die Konsole und gibt die Antwort aus
  ./vmctl.py shot <datei.png>            Bildschirmfoto des VM-Bildschirms
  ./vmctl.py key <taste> [...]           Tasten im VM-Fenster drücken (QEMU-Namen: ret, a, spc, ...),
                                         z. B. ret für die Anmeldung des Desktops
  ./vmctl.py click <x> <y> [taste]       Desktop: Mausklick an einer Stelle (1 links, 3 rechts)
  ./vmctl.py move <x> <y>                Desktop: Mauszeiger an eine Stelle
  ./vmctl.py xkey <name> [...]           Desktop: Tasten über X (Return, Escape, Down, a, ...), auch mit Umschalttasten: ctrl+s, ctrl+shift+s, alt+F4
  ./vmctl.py type '<text>'               Desktop: Text tippen
  ./vmctl.py quit                        VM ausschalten

click, move, xkey und type laufen über die serielle Konsole (als root) mit clokeos-xtest aus
CloakServer: Die VM hat nur eine relative USB-Maus, die sich von außen nicht genau setzen lässt.

Für eine arm64-VM mit CLOKEOS_ARCH=arm64 aufrufen, wie run.sh.
"""
import json
import os
import re
import shlex
import socket
import sys
import time

STATE = os.path.join(os.environ.get('CLOKEOS_WORK', os.path.expanduser('~/ClokeOS-work')), 'vm',
                     os.environ.get('CLOKEOS_ARCH', 'x86_64'))
SERIAL_LOG = os.path.join(STATE, 'serial.log')
PROMPT = re.compile(rb'[#%] $')


def qmp(*cmds):
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.path.join(STATE, 'qmp.sock'))
    f = s.makefile('rwb')
    json.loads(f.readline())
    replies = []
    for cmd in [{'execute': 'qmp_capabilities'}, *cmds]:
        f.write(json.dumps(cmd).encode() + b'\n')
        f.flush()
        while True:
            msg = json.loads(f.readline())
            if 'return' in msg or 'error' in msg:
                replies.append(msg)
                break
    s.close()
    return replies[1:]


def wait(pattern, timeout=600):
    rx = re.compile(pattern.encode())
    end = time.time() + timeout
    while time.time() < end:
        with open(SERIAL_LOG, 'rb') as f:
            m = rx.search(f.read())
        if m:
            print(m.group(0).decode(errors='replace'))
            return 0
        time.sleep(1)
    print('Zeitüberschreitung beim Warten auf', pattern, file=sys.stderr)
    return 1


def send(command, timeout=60):
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.path.join(STATE, 'serial.sock'))
    s.settimeout(0.5)
    # Langsam tippen: die emulierte serielle Schnittstelle verliert sonst Zeichen.
    for ch in command.encode():
        s.send(bytes([ch]))
        time.sleep(0.01)
    s.send(b'\r')
    buf = b''
    end = time.time() + timeout
    while time.time() < end:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            chunk = b''
        buf += chunk
        lines = buf.split(b'\n')
        if len(lines) > 1 and PROMPT.search(lines[-1]):
            break
    s.close()
    text = buf.replace(b'\r', b'').decode(errors='replace').split('\n')
    print('\n'.join(text[1:-1]))  # ohne Echo des Befehls und ohne neue Eingabeaufforderung
    return 0


def shot(path):
    path = os.path.abspath(path)
    r = qmp({'execute': 'screendump', 'arguments': {'filename': path, 'format': 'png'}})
    if 'error' in r[0]:
        print(r[0]['error']['desc'], file=sys.stderr)
        return 1
    print(path)
    return 0


def key(names):
    for n in names:
        r = qmp({'execute': 'send-key', 'arguments': {'keys': [{'type': 'qcode', 'data': n}]}})
        if 'error' in r[0]:
            print(r[0]['error']['desc'], file=sys.stderr)
            return 1
        time.sleep(.1)
    return 0


XTEST = '/System/Library/CoreServices/CloakServer/clokeos-xtest'


def xtest(args):
    return send('DISPLAY=:0 %s %s' % (XTEST, ' '.join(shlex.quote(a) for a in args)), 30)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd, rest = argv[1], argv[2:]
    if cmd == 'wait':
        return wait(rest[0], float(rest[1]) if len(rest) > 1 else 600)
    if cmd == 'send':
        return send(rest[0], float(rest[1]) if len(rest) > 1 else 60)
    if cmd == 'shot':
        return shot(rest[0])
    if cmd == 'key' and rest:
        return key(rest)
    if cmd in ('click', 'move') and len(rest) >= 2:
        return xtest([cmd] + rest)
    if cmd == 'xkey' and rest:
        return xtest(['key'] + rest)
    if cmd == 'type' and rest:
        return xtest(['type', rest[0]])
    if cmd == 'quit':
        qmp({'execute': 'quit'})
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
