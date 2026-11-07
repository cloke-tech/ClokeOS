#!/usr/bin/env python3
"""Hört auf TCP-Port 8222 und speichert, was clokeos-livecall vom Live-Stick (recovery/usb.sh live) schickt.

Pro Verbindung eine Datei ~/ClokeOS-work/live/hello-<ip>-<zeit>.txt; auf der Standardausgabe steht je
Meldung eine Zeile "HELLO ip=… if=… host=…" (daran hängt sich ein Monitor), danach die ssh-Zeile.
Aufruf: recovery/live-listen.py [ordner]
"""
import os, socket, sys, threading, time

PORT = 8222
out = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/ClokeOS-work/live")
os.makedirs(out, exist_ok=True)

def handle(conn, addr):
    conn.settimeout(20)
    data = b""
    try:
        while len(data) < 8 * 1024 * 1024:
            b = conn.recv(65536)
            if not b:
                break
            data += b
    except OSError:
        pass
    conn.close()
    head = data.split(b"\n", 1)[0].decode("utf-8", "replace")
    if not head.startswith("HELLO"):
        return
    name = os.path.join(out, "hello-%s-%d.txt" % (addr[0], time.time()))
    if "full=1" in head:
        open(name, "wb").write(data)
    print(head, "saved=%s" % (name if "full=1" in head else "-"), flush=True)
    ip = head.split("ip=")[1].split()[0]
    print("ssh -i %s/id_clokeos_live -o StrictHostKeyChecking=no root@%s" % (out, ip), flush=True)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", PORT))
srv.listen(16)
print("waiting on port %d" % PORT, flush=True)
while True:
    c, a = srv.accept()
    threading.Thread(target=handle, args=(c, a), daemon=True).start()
