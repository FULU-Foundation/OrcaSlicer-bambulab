#!/usr/bin/env python3
"""Guest side of the PJarczak bridge transport.

Connects back to the host-side relay through passt's host-loopback mapping
(the guest's default gateway address is translated to the host's 127.0.0.1),
then runs the x86-64 Bambu network host with pipes as stdin/stdout and pumps
frames between those pipes and the socket.

Pipes matter: the host re-opens its stdout through /proc/self/fd/N, and that
only works for regular files and pipes, not for sockets.
"""
import os
import socket
import subprocess
import sys
import threading
import time

port = int(sys.argv[1])
host = sys.argv[2]
log = sys.argv[3]
gateway = sys.argv[4]

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
for _ in range(300):
    try:
        sock.connect((gateway, port))
        break
    except OSError:
        time.sleep(0.1)
else:
    sys.stderr.write("bridge relay: cannot connect to %s:%d\n" % (gateway, port))
    sys.exit(1)

to_host_r, to_host_w = os.pipe()
from_host_r, from_host_w = os.pipe()

with open(log, "ab", 0) as lf:
    lf.write(("relay: connected to %s:%d\n" % (gateway, port)).encode())
    proc = subprocess.Popen([host], stdin=to_host_r, stdout=from_host_w,
                            stderr=lf, close_fds=True)
    lf.write(b"relay: spawned host\n")
    os.close(to_host_r)
    os.close(from_host_w)

    def socket_to_host():
        try:
            while True:
                data = sock.recv(65536)
                if not data:
                    break
                os.write(to_host_w, data)
        except OSError:
            pass
        finally:
            try:
                os.close(to_host_w)
            except OSError:
                pass

    def host_to_socket():
        try:
            while True:
                data = os.read(from_host_r, 65536)
                if not data:
                    break
                sock.sendall(data)
        except OSError:
            pass
        finally:
            try:
                sock.shutdown(socket.SHUT_WR)
            except OSError:
                pass

    readers = [threading.Thread(target=socket_to_host, daemon=True),
               threading.Thread(target=host_to_socket, daemon=True)]
    for t in readers:
        t.start()

    proc.wait()
    for t in readers:
        t.join(timeout=1.0)
    sock.close()
