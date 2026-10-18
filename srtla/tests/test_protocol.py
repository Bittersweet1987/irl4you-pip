"""Protocol tests: irl4you-srtla against the test receiver, all on this machine.

Every uplink is its own loopback address (127.0.0.2, 127.0.0.3, ...), so the
receiver can tell them apart and make single ones slow, lossy or dead.
"""
import hashlib
import importlib.util
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.environ.get("IRL4YOU_SRTLA_BIN") or os.path.join(ROOT, "build", "irl4you-srtla")

_spec = importlib.util.spec_from_file_location("testrecv", os.path.join(ROOT, "testrecv", "irl4you-srtla-testrecv.py"))
testrecv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(testrecv)


def setUpModule():
    if "IRL4YOU_SRTLA_BIN" not in os.environ:
        subprocess.run(["make", "-s", "-C", ROOT], check=True)


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def data_pkt(seq, payload=b""):
    """A minimal SRT data packet: sequence number, message word, timestamp, socket id."""
    return struct.pack(">IIII", seq & 0x7FFFFFFF, 0xC0000000, 0, 1) + (payload or b"x" * 1300)


def ctl_pkt(ctl, body=b"", extra=0):
    return struct.pack(">IIII", 0x80000000 | (ctl << 16), extra, 0, 1) + body


class Rig:
    """Sender + receiver + fake encoder (client) + fake SRT server (target)."""

    def __init__(self, ips, args=(), receiver_kw=None):
        self.tmp = tempfile.mkdtemp(prefix="srtla-test-")
        self.server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.settimeout(0.2)
        self.from_server_lock = threading.Lock()
        self.at_server = []
        self.ctl_at_server = []
        self.collecting = True
        self.rx = testrecv.Receiver(listen=("127.0.0.1", 0), target=self.server.getsockname(),
                                    **(receiver_kw or {})).start()
        self.links_file = os.path.join(self.tmp, "links")
        self.write_links(ips)
        self.status_file = os.path.join(self.tmp, "status.json")
        self.listen_port = free_port()
        self.out = []
        self.proc = subprocess.Popen(
            [BIN, "-s", f"127.0.0.1:{self.rx.port}", "-i", self.links_file, "-l", f"127.0.0.1:{self.listen_port}",
             "--status", self.status_file, *args],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        threading.Thread(target=self._read_out, daemon=True).start()
        threading.Thread(target=self._collect, daemon=True).start()
        self.client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.client.bind(("127.0.0.1", 0))
        self.client.settimeout(0.2)

    def _read_out(self):
        for line in self.proc.stdout:
            self.out.append(line)

    def _collect(self):
        while self.collecting:
            try:
                buf, _ = self.server.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            with self.from_server_lock:
                if buf[0] & 0x80:
                    self.ctl_at_server.append(buf)
                else:
                    self.at_server.append(struct.unpack_from(">I", buf)[0])

    def events(self, name):
        res = []
        for line in list(self.out):
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            if ev.get("ev") == name:
                res.append(ev)
        return res

    def write_links(self, lines):
        with open(self.links_file, "w") as f:
            f.write("# Leitungen\n" + "\n".join(lines) + "\n")

    def status(self):
        try:
            with open(self.status_file) as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def link(self, ip):
        st = self.status() or {}
        return next((l for l in st.get("links", []) if l["ip"] == ip), None)

    def wait(self, cond, timeout=8.0, msg="Bedingung"):
        end = time.time() + timeout
        while time.time() < end:
            if cond():
                return
            time.sleep(0.05)
        raise AssertionError(f"{msg} nicht erreicht; Ausgabe:\n" + "".join(self.out[-30:]))

    def wait_up(self, ips, timeout=8.0):
        self.wait(lambda: all((self.link(ip) or {}).get("state") in ("used", "reserve") for ip in ips), timeout,
                  f"Leitungen {ips} angemeldet")

    def wait_rtt(self, ips, timeout=8.0):
        self.wait(lambda: all((self.link(ip) or {}).get("rtt_ms") is not None for ip in ips), timeout,
                  f"Laufzeit für {ips}")

    def send(self, seq, payload=b""):
        self.client.sendto(data_pkt(seq, payload), ("127.0.0.1", self.listen_port))

    def stream(self, first, count, pace=0.0005):
        for i in range(count):
            self.send(first + i)
            if pace:
                time.sleep(pace)

    def drain_server(self, timeout=1.0):
        """Data sequence numbers that reached the SRT server since the last call."""
        time.sleep(timeout)
        with self.from_server_lock:
            seqs, self.at_server = self.at_server, []
        return seqs

    def stop_collecting(self):
        self.collecting = False
        time.sleep(0.3)

    def close(self):
        self.collecting = False
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.rx.stop()
        self.server.close()
        self.client.close()
        shutil.rmtree(self.tmp, ignore_errors=True)


class Base(unittest.TestCase):
    def rig(self, ips, *args, **kw):
        r = Rig(ips, args, **kw)
        self.addCleanup(r.close)
        return r


class TestRegistration(Base):
    def test_group_and_all_links_join(self):
        r = self.rig(["127.0.0.2", "127.0.0.3 lo", "127.0.0.4"])
        r.wait_up(["127.0.0.2", "127.0.0.3", "127.0.0.4"])
        self.assertEqual(r.rx.group_count(), 1)
        self.assertEqual(r.rx.conn_ips(), ["127.0.0.2", "127.0.0.3", "127.0.0.4"])
        self.assertEqual(len(r.events("group_ok")), 1)
        self.assertEqual(r.link("127.0.0.3")["dev"], "lo")

    def test_retries_until_receiver_answers(self):
        r = self.rig(["127.0.0.2"])
        r.rx.set_rule("127.0.0.2", dead=True)
        time.sleep(2.5)
        self.assertEqual(r.rx.group_count(), 0)
        r.rx.clear_rules()
        r.wait_up(["127.0.0.2"], timeout=5)

    def test_group_refused_then_accepted(self):
        r = self.rig(["127.0.0.2"])
        r.rx.refuse_groups = True
        time.sleep(1.5)
        self.assertTrue(any(e[0] == "group_nak" for e in r.rx.log) or r.rx.group_count() == 1)
        r.rx.refuse_groups = False
        r.wait_up(["127.0.0.2"])

    def test_receiver_restart_builds_new_group(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"])
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        r.rx.forget_all()
        # links fall silent, join again, get "group unknown", start over
        r.wait(lambda: len(r.events("group_ok")) >= 2, timeout=12, msg="neue Gruppe")
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        self.assertEqual(r.rx.group_count(), 1)
        r.stream(1, 200)
        self.assertGreater(len(r.drain_server()), 150)

    def test_bad_arguments(self):
        for args in ([], ["-s", "x"], ["-s", "h:1", "-i", "/x", "--min-share", "50"], ["-s", "h:1", "-i", "/x", "-m", "fast"]):
            p = subprocess.run([BIN, *args], capture_output=True, text=True)
            self.assertEqual(p.returncode, 2, args)
            self.assertIn("irl4you-srtla", p.stderr)

    def test_version(self):
        p = subprocess.run([BIN, "--version"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0)
        self.assertTrue(p.stdout.startswith("irl4you-srtla "))


class TestForwarding(Base):
    def test_everything_arrives_in_both_directions(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "all")
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        r.stream(1000, 3000)
        got = r.drain_server(1.5)
        self.assertEqual(sorted(set(got)), list(range(1000, 4000)))
        # answers of the server reach the encoder
        group_addr = next(iter(r.rx.groups.values())).sock.getsockname()
        r.server.sendto(ctl_pkt(5, b"", 7), group_addr)
        r.client.settimeout(1)
        buf, _ = r.client.recvfrom(4096)
        self.assertEqual(struct.unpack_from(">I", buf)[0], 0x80050000)

    def test_rx_lists_free_the_windows(self):
        r = self.rig(["127.0.0.2"])
        r.wait_up(["127.0.0.2"])
        r.stream(1, 1000)
        time.sleep(1.5)
        l = r.link("127.0.0.2")
        self.assertLess(l["inflight"], 10)

    def test_ack_on_every_link_reaches_encoder_once(self):
        r = self.rig(["127.0.0.2", "127.0.0.3", "127.0.0.4"])
        r.wait_up(["127.0.0.2", "127.0.0.3", "127.0.0.4"])
        r.send(1)  # let the sender learn the encoder address
        time.sleep(0.2)
        ack = ctl_pkt(2, struct.pack(">IIII", 1234, 20000, 5000, 100), 42)
        r.rx.send_to_conns(ack)
        got = []
        try:
            while True:
                buf, _ = r.client.recvfrom(4096)
                got.append(buf)
        except socket.timeout:
            pass
        self.assertEqual(got.count(ack), 1)
        # a different ACK passes again
        ack2 = ctl_pkt(2, struct.pack(">IIII", 1300, 20000, 5000, 100), 43)
        r.rx.send_to_conns(ack2)
        buf, _ = r.client.recvfrom(4096)
        self.assertEqual(buf, ack2)

    def test_control_packets_from_encoder_go_through(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"])
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        hs = ctl_pkt(0, b"h" * 48)
        r.client.sendto(hs, ("127.0.0.1", r.listen_port))
        r.wait(lambda: hs in r.ctl_at_server, timeout=2, msg="Handshake beim Server")


class TestLinkChoice(Base):
    def test_rtt_measured_per_link(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"])
        r.rx.set_rule("127.0.0.3", delay_ms=150)
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        time.sleep(3)
        self.assertLess(r.link("127.0.0.2")["rtt_ms"], 30)
        self.assertAlmostEqual(r.link("127.0.0.3")["rtt_ms"], 150, delta=40)

    def test_best_mode_keeps_slow_link_as_reserve(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "best")
        r.rx.set_rule("127.0.0.3", delay_ms=200)
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        time.sleep(2.5)
        self.assertEqual(r.link("127.0.0.3")["state"], "reserve")
        self.assertEqual(r.link("127.0.0.2")["state"], "used")
        r.stream(1, 2000)
        r.drain_server(0.5)
        self.assertGreater(r.rx.data_by_ip.get("127.0.0.2", 0), 1900)
        self.assertLess(r.rx.data_by_ip.get("127.0.0.3", 0), 100)

    def test_all_mode_uses_slower_link_within_margin(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "all")
        r.rx.set_rule("127.0.0.3", delay_ms=60)
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        time.sleep(2.5)
        self.assertEqual(r.link("127.0.0.3")["state"], "used")
        r.stream(1, 3000)
        r.drain_server(0.5)
        self.assertGreater(r.rx.data_by_ip.get("127.0.0.3", 0), 300)

    def test_margin_decides_reserve_in_all_mode(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "all", "--margin", "50")
        r.rx.set_rule("127.0.0.3", delay_ms=200)
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        time.sleep(2.5)
        self.assertEqual(r.link("127.0.0.3")["state"], "reserve")

    def test_no_latency_uses_every_link(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "all", "--no-latency", "--min-share", "0")
        r.rx.set_rule("127.0.0.3", delay_ms=400)
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        time.sleep(2)
        self.assertEqual(r.link("127.0.0.3")["state"], "used")

    def test_bursts_reduce_reordering(self):
        def gaps(*args):
            r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "all", "--margin", "100", *args)
            r.rx.set_rule("127.0.0.3", delay_ms=15)
            r.wait_up(["127.0.0.2", "127.0.0.3"])
            r.wait_rtt(["127.0.0.2", "127.0.0.3"])
            time.sleep(2)
            r.stream(1, 3000, pace=0.001)
            seqs = r.drain_server(0.6)
            self.assertGreater(len(seqs), 2900)
            self.assertGreater(r.rx.data_by_ip.get("127.0.0.3", 0), 300, "both links must carry traffic")
            # what an SRT receiver would NAK: every packet that jumps past the highest number seen so far
            top, jumps = 0, 0
            for q in seqs:
                if top and q > top + 1:
                    jumps += 1
                top = max(top, q)
            return jumps

        single, burst = gaps("--burst", "1"), gaps("--burst", "12")
        self.assertLess(burst, single * 0.5, f"single={single} burst={burst}")

    def test_min_share(self):
        def share_of_second(*args):
            r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "best", "--margin", "100", *args)
            r.rx.set_rule("127.0.0.3", delay_ms=30)
            r.wait_up(["127.0.0.2", "127.0.0.3"])
            r.wait_rtt(["127.0.0.2", "127.0.0.3"])
            time.sleep(2)
            r.stream(1, 3000)
            r.drain_server(0.5)
            n2, n3 = r.rx.data_by_ip.get("127.0.0.2", 0), r.rx.data_by_ip.get("127.0.0.3", 0)
            return n3 / max(1, n2 + n3)

        self.assertLess(share_of_second(), 0.05)
        self.assertGreater(share_of_second("--min-share", "25"), 0.18)

    def test_fresh_gap_is_reordering_not_loss(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "all", "--no-latency", "--min-share", "30")
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        r.wait_rtt(["127.0.0.2", "127.0.0.3"])
        r.rx.set_rule("127.0.0.3", delay_ms=500)
        r.stream(1, 200, pace=0)
        not_yet = [s for s in range(1, 201) if s not in r.rx.seq_ip]
        self.assertTrue(not_yet, "nichts unterwegs")
        r.rx.send_to_conns(ctl_pkt(3, b"".join(struct.pack(">I", s) for s in not_yet[-3:])), ip="127.0.0.2")
        both = ("127.0.0.2", "127.0.0.3")
        r.wait(lambda: sum(r.link(i)["reordered"] for i in both) >= 1, timeout=3, msg="Umordnung gezählt")
        self.assertEqual(sum(r.link(i)["lost"] for i in both), 0)

    def test_loss_report_counts_on_the_right_link(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "all", "--no-latency", "--min-share", "30")
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        r.rx.set_rule("127.0.0.3", delay_ms=500)  # packets over .3 stay on their way for a while
        r.stream(1, 400, pace=0.001)
        time.sleep(0.1)
        not_yet = [s for s in range(1, 401) if s not in r.rx.seq_ip]
        self.assertTrue(not_yet, "nichts über .3 gesendet")
        window = r.link("127.0.0.3")["window"]
        time.sleep(0.15)  # older than the reordering window, still on its way: a real loss
        body = b"".join(struct.pack(">I", s) for s in not_yet[:60])
        r.rx.send_to_conns(ctl_pkt(3, body), ip="127.0.0.2")
        r.wait(lambda: r.link("127.0.0.3")["lost"] >= 1, timeout=3, msg="Verlust auf .3")
        self.assertEqual(r.link("127.0.0.2")["lost"], 0)


class TestFailure(Base):
    def test_dead_link_is_dropped_and_traffic_moves(self):
        r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "all")
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        r.rx.set_rule("127.0.0.2", dead=True)
        r.wait(lambda: r.link("127.0.0.2")["state"] in ("joining", "down"), timeout=7, msg=".2 getrennt")
        r.stream(5000, 1000)
        got = r.drain_server(1.0)
        self.assertEqual(len(set(got)), 1000)
        r.rx.clear_rules()
        r.wait_up(["127.0.0.2"], timeout=6)

    def test_missing_address_is_retried(self):
        r = self.rig(["192.0.2.77", "127.0.0.2"])
        r.wait_up(["127.0.0.2"])
        self.assertEqual(r.link("192.0.2.77")["state"], "down")
        self.assertIn("error", r.link("192.0.2.77"))

    def test_reload_adds_and_removes_links(self):
        r = self.rig(["127.0.0.2"])
        r.wait_up(["127.0.0.2"])
        r.write_links(["127.0.0.3", "127.0.0.4"])
        r.proc.send_signal(signal.SIGHUP)
        r.wait_up(["127.0.0.3", "127.0.0.4"])
        r.wait(lambda: r.link("127.0.0.2") is None, msg=".2 entfernt")
        r.stream(1, 300)
        got = r.drain_server(0.8)
        self.assertEqual(len(set(got)), 300)
        self.assertNotIn("127.0.0.2", [ip for ip, n in r.rx.data_by_ip.items() if n and ip == "127.0.0.2"])

    def test_clean_stop(self):
        r = self.rig(["127.0.0.2"])
        r.wait_up(["127.0.0.2"])
        r.proc.send_signal(signal.SIGTERM)
        self.assertEqual(r.proc.wait(3), 0)


@unittest.skipUnless(shutil.which("srt-live-transmit"), "srt-live-transmit fehlt (Paket srt-tools)")
class TestRealSrt(Base):
    """A real SRT connection (libsrt on both ends) through sender and receiver."""

    def run_transfer(self, rules, size=4_000_000):
        r = self.rig(["127.0.0.2", "127.0.0.3"], "-m", "all")
        for ip, kw in rules.items():
            r.rx.set_rule(ip, **kw)
        r.wait_up(["127.0.0.2", "127.0.0.3"])
        # the fake SRT server socket is replaced by a real SRT listener on the same port
        srt_port = r.server.getsockname()[1]
        r.stop_collecting()
        r.server.close()
        out_port = free_port()
        sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sink.bind(("127.0.0.1", out_port))
        sink.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
        sink.settimeout(0.5)
        in_port = free_port()
        lst = subprocess.Popen(["srt-live-transmit", f"srt://:{srt_port}?mode=listener&latency=1000",
                                f"udp://127.0.0.1:{out_port}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lst.kill)
        time.sleep(0.5)
        call = subprocess.Popen(["srt-live-transmit", f"udp://127.0.0.1:{in_port}",
                                 f"srt://127.0.0.1:{r.listen_port}?mode=caller&latency=1000"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(call.kill)
        time.sleep(1.5)
        data = os.urandom(size)
        chunks = [data[i:i + 1316] for i in range(0, len(data), 1316)]
        received = []

        def collect():
            idle = 0
            while idle < 6:
                try:
                    buf, _ = sink.recvfrom(2048)
                    received.append(buf)
                    idle = 0
                except socket.timeout:
                    idle += 1

        t = threading.Thread(target=collect)
        t.start()
        src = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        start = time.time()
        for i, c in enumerate(chunks):  # about 8 Mbit/s
            src.sendto(c, ("127.0.0.1", in_port))
            delay = start + (i + 1) * 1316 * 8 / 8e6 - time.time()
            if delay > 0:
                time.sleep(delay)
        t.join(20)
        src.close()
        sink.close()
        return r, hashlib.sha256(data).hexdigest(), hashlib.sha256(b"".join(received)).hexdigest()

    def test_clean_transfer(self):
        r, want, got = self.run_transfer({})
        self.assertEqual(want, got)
        self.assertGreater(r.rx.data_by_ip.get("127.0.0.2", 0), 100)
        self.assertGreater(r.rx.data_by_ip.get("127.0.0.3", 0), 100)

    def test_lossy_link_is_repaired_by_srt(self):
        r, want, got = self.run_transfer({"127.0.0.3": {"loss": 0.05, "delay_ms": 20}})
        self.assertEqual(want, got)
        self.assertGreater(r.link("127.0.0.3")["lost"], 0)


if __name__ == "__main__":
    unittest.main()
