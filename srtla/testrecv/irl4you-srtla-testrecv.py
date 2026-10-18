#!/usr/bin/env python3
"""Test receiver for irl4you-srtla.

Plays the role of the SRTLA receiver on the server side (see ../PROTOKOLL.md):
groups uplinks, forwards SRT to a target and sends answers back. On top of that it
can make single uplinks slow, lossy or dead, so the sender can be tested without the
box and without a foreign service.

Use as a program:
    irl4you-srtla-testrecv.py --listen 0.0.0.0:5000 --target 127.0.0.1:4001 \
        --rule 10.0.0.2:delay=120,loss=0.05
or import it and use Receiver directly (see ../tests).
"""
import argparse
import heapq
import os
import random
import selectors
import socket
import struct
import sys
import threading
import time

ALIVE, RXLIST = 0x9000, 0x9100
GROUP_REQ, JOIN, JOINED = 0x9200, 0x9201, 0x9202
JOIN_ERR, NO_GROUP, GROUP_NAK = 0x9210, 0x9211, 0x9212
KEY_LEN = 256
SRT_ACK = 2
RXLIST_EVERY = 10


class Rule:
    def __init__(self, delay_ms=0.0, loss=0.0, dead=False):
        self.delay_ms, self.loss, self.dead = delay_ms, loss, dead


class Conn:
    def __init__(self, addr, now):
        self.addr = addr
        self.last = now
        self.pending = []
        self.data = 0
        self.seqs = []


class Group:
    def __init__(self, key, target, now):
        self.key = key
        self.conns = {}
        self.last_conn = None
        self.empty_since = now
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)
        self.sock.connect(target)


class Receiver:
    def __init__(self, listen=("127.0.0.1", 0), target=("127.0.0.1", 4001), max_conns=16, max_groups=64,
                 conn_timeout=10.0, group_timeout=10.0, echo_unknown_alive=False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 21)
        self.sock.bind(listen)
        self.sock.setblocking(False)
        self.port = self.sock.getsockname()[1]
        self.target = target
        self.max_conns, self.max_groups = max_conns, max_groups
        self.conn_timeout, self.group_timeout = conn_timeout, group_timeout
        self.echo_unknown_alive = echo_unknown_alive
        self.groups = {}
        self.by_addr = {}
        self.rules = {}
        self.refuse_groups = False
        self.lock = threading.RLock()
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.sock, selectors.EVENT_READ, None)
        self.timers = []
        self.tick = 0
        self.stop_ev = threading.Event()
        self.thread = None
        # what the tests look at
        self.log = []
        self.data_by_ip = {}
        self.seq_ip = {}
        self.forwarded = 0

    # ---- control for tests ----
    def set_rule(self, ip, **kw):
        with self.lock:
            self.rules[ip] = Rule(**kw)

    def clear_rules(self):
        with self.lock:
            self.rules.clear()

    def forget_all(self):
        """Drop every group, as if the receiver had been restarted."""
        with self.lock:
            for g in self.groups.values():
                self.sel.unregister(g.sock)
                g.sock.close()
            self.groups.clear()
            self.by_addr.clear()
            self.log.append(("forget",))

    def group_count(self):
        with self.lock:
            return len(self.groups)

    def conn_ips(self):
        with self.lock:
            return sorted(a[0] for g in self.groups.values() for a in g.conns)

    def send_to_conns(self, payload, ip=None):
        """Send raw bytes to every connection (or those of one ip), like an answer from the server."""
        with self.lock:
            for g in self.groups.values():
                for a in g.conns:
                    if ip is None or a[0] == ip:
                        self._out(payload, a)

    # ---- running ----
    def start(self):
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
        return self

    def stop(self):
        self.stop_ev.set()
        if self.thread:
            self.thread.join(2)
        self.sock.close()

    def run(self):
        while not self.stop_ev.is_set():
            now = time.monotonic()
            wait = 0.05
            if self.timers:
                wait = max(0.0, min(wait, self.timers[0][0] - now))
            for key, _ in self.sel.select(wait):
                with self.lock:
                    self._readable(key)
            now = time.monotonic()
            with self.lock:
                while self.timers and self.timers[0][0] <= now:
                    _, _, fn, args = heapq.heappop(self.timers)
                    fn(*args)
                if now - self.tick > 0.5:
                    self.tick = now
                    self._expire(now)

    def _later(self, delay, fn, *args):
        self.tie = getattr(self, "tie", 0) + 1
        heapq.heappush(self.timers, (time.monotonic() + delay, self.tie, fn, args))

    def _rule(self, ip):
        return self.rules.get(ip)

    def _out(self, payload, addr):
        r = self._rule(addr[0])
        if r and r.dead:
            return
        try:
            self.sock.sendto(payload, addr)
        except OSError:
            pass

    def _readable(self, key):
        sock = key.fileobj
        for _ in range(512):
            try:
                if key.data is None:
                    buf, addr = sock.recvfrom(4096)
                else:
                    buf = sock.recv(4096)
            except (BlockingIOError, ConnectionRefusedError):
                return
            except OSError:
                return
            if key.data is None:
                r = self._rule(addr[0])
                if r and (r.dead or (r.loss and random.random() < r.loss)):
                    continue
                if r and r.delay_ms:
                    self._later(r.delay_ms / 1000.0, self._from_sender, buf, addr)
                else:
                    self._from_sender(buf, addr)
            else:
                self._from_target(key.data, buf)

    def _from_target(self, key, buf):
        g = self.groups.get(key)
        if not g:
            return
        if len(buf) >= 16 and buf[0] & 0x80 and (struct.unpack_from(">H", buf)[0] & 0x7FFF) == SRT_ACK:
            for a in g.conns:
                self._out(buf, a)
        elif g.last_conn in g.conns:
            self._out(buf, g.last_conn)

    def _from_sender(self, buf, addr):
        now = time.monotonic()
        if len(buf) < 2:
            return
        t = struct.unpack_from(">H", buf)[0]
        if t == GROUP_REQ and len(buf) == 2 + KEY_LEN:
            if self.refuse_groups or len(self.groups) >= self.max_groups:
                self._out(struct.pack(">H", GROUP_NAK), addr)
                self.log.append(("group_nak", addr[0]))
                return
            key = buf[2:2 + KEY_LEN // 2] + os.urandom(KEY_LEN // 2)
            g = Group(key, self.target, now)
            self.groups[key] = g
            self.sel.register(g.sock, selectors.EVENT_READ, key)
            self.log.append(("group", addr[0]))
            self._out(struct.pack(">H", JOIN) + key, addr)
            return
        if t == JOIN and len(buf) == 2 + KEY_LEN:
            g = self.groups.get(buf[2:])
            if not g:
                self._out(struct.pack(">H", NO_GROUP), addr)
                self.log.append(("no_group", addr[0]))
                return
            if addr not in g.conns:
                if len(g.conns) >= self.max_conns:
                    self._out(struct.pack(">H", JOIN_ERR), addr)
                    return
                old = self.by_addr.get(addr)
                if old is not None and old is not g:
                    old.conns.pop(addr, None)
                g.conns[addr] = Conn(addr, now)
                self.by_addr[addr] = g
                self.log.append(("join", addr[0]))
            g.conns[addr].last = now
            self._out(struct.pack(">H", JOINED), addr)
            return
        g = self.by_addr.get(addr)
        if t == ALIVE:
            if g or self.echo_unknown_alive:
                if g:
                    g.conns[addr].last = now
                self._out(buf, addr)
            return
        if not g or 0x9000 <= t <= 0x92FF:
            return
        c = g.conns[addr]
        c.last = now
        g.last_conn = addr
        if not buf[0] & 0x80 and len(buf) >= 4:
            seq = struct.unpack_from(">I", buf)[0] & 0x7FFFFFFF
            c.seqs.append(seq)
            self.data_by_ip[addr[0]] = self.data_by_ip.get(addr[0], 0) + 1
            self.seq_ip[seq] = addr[0]
            if len(c.seqs) >= RXLIST_EVERY:
                self._out(struct.pack(">HH", RXLIST, 0) + b"".join(struct.pack(">I", s) for s in c.seqs), addr)
                c.seqs = []
        try:
            g.sock.send(buf)
            self.forwarded += 1
        except OSError:
            pass

    def _expire(self, now):
        for key, g in list(self.groups.items()):
            for a, c in list(g.conns.items()):
                if now - c.last > self.conn_timeout:
                    del g.conns[a]
                    self.by_addr.pop(a, None)
                    self.log.append(("conn_timeout", a[0]))
            if g.conns:
                g.empty_since = now
            elif now - g.empty_since > self.group_timeout:
                self.sel.unregister(g.sock)
                g.sock.close()
                del self.groups[key]
                self.log.append(("group_timeout",))


def host_port(s):
    host, _, port = s.rpartition(":")
    return host, int(port)


def main():
    ap = argparse.ArgumentParser(description="SRTLA-Test-Empfänger für irl4you-srtla")
    ap.add_argument("--listen", type=host_port, default=("0.0.0.0", 5000), help="ADR:PORT für die Leitungen")
    ap.add_argument("--target", type=host_port, required=True, help="ADR:PORT des SRT-Servers")
    ap.add_argument("--rule", action="append", default=[],
                    help="IP:delay=MS,loss=0..1,dead=1 macht eine Leitung langsam, verlustreich oder tot")
    ap.add_argument("--stats", type=float, default=2.0, help="alle N Sekunden Zähler ausgeben (0 = nie)")
    a = ap.parse_args()
    rx = Receiver(listen=a.listen, target=a.target)
    for r in a.rule:
        ip, _, opts = r.partition(":")
        kw = {}
        for kv in filter(None, opts.split(",")):
            k, _, v = kv.partition("=")
            kw[{"delay": "delay_ms"}.get(k, k)] = (v == "1") if k == "dead" else float(v)
        rx.set_rule(ip, **kw)
    rx.start()
    print(f"Test-Empfänger auf Port {rx.port}, SRT-Ziel {a.target[0]}:{a.target[1]}", flush=True)
    try:
        while True:
            time.sleep(a.stats or 3600)
            if a.stats:
                print(f"Gruppen={rx.group_count()} Leitungen={rx.conn_ips()} Pakete je IP={rx.data_by_ip} "
                      f"weitergegeben={rx.forwarded}", flush=True)
    except KeyboardInterrupt:
        pass
    rx.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
