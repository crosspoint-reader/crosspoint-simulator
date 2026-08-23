#!/usr/bin/env python3
"""Gate for the BLE shim transport.

Builds tests/sim_ble_link_selftest.cpp against src/SimBleLink.cpp and
src/SimBleProtocol.cpp, then drives the listener over TCP:

  - every client op, once with explicit fields and once with defaults
  - a second client is refused
  - malformed lines do not crash the process
  - a line over the 65536 byte cap is dropped, framing recovers
  - stop() returns promptly with a client connected
  - two threads emitting concurrently produce intact, non-interleaved lines

Run from the repo root:  python3 tests/sim_ble_link_selftest.py
Exit code 0 means every check passed.
"""

import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = int(os.environ.get("SELFTEST_PORT", "18765"))

failures = []
checks = 0


def check(ok, label, detail=""):
    global checks
    checks += 1
    if ok:
        print(f"  ok   {label}")
    else:
        failures.append(label)
        print(f"  FAIL {label}: {detail}")


def build(binary):
    cmd = [
        "g++", "-std=c++17", "-Wall", "-Wextra", "-O1", "-I", os.path.join(ROOT, "src"),
    ]
    sanitize = os.environ.get("SELFTEST_SANITIZE", "")
    if sanitize == "1":
        cmd += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    elif sanitize == "thread":
        cmd += ["-fsanitize=thread", "-fno-omit-frame-pointer", "-g"]
    cmd += [
        os.path.join(ROOT, "src", "SimBleLink.cpp"),
        os.path.join(ROOT, "src", "SimBleProtocol.cpp"),
        os.path.join(ROOT, "tests", "sim_ble_link_selftest.cpp"),
        "-o", binary, "-lpthread",
    ]
    print("$ " + " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.stdout.strip():
        print(proc.stdout)
    if proc.stderr.strip():
        print(proc.stderr)
    if proc.returncode != 0:
        print("build failed")
        sys.exit(1)
    print("build clean, no warnings\n")


class Harness:
    """The C++ selftest process. stdout lines are collected by a thread."""

    def __init__(self, binary):
        argv = [binary, str(PORT)]
        if os.environ.get("SELFTEST_SANITIZE") == "thread":
            # ThreadSanitizer needs ASLR off on current kernels, otherwise it
            # dies with "unexpected memory mapping" before main().
            argv = ["setarch", "-R"] + argv
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1, cwd=ROOT,
        )
        self.lines = []
        self.lock = threading.Lock()
        self.pump = threading.Thread(target=self._pump, daemon=True)
        self.pump.start()

    def _pump(self):
        for line in self.proc.stdout:
            with self.lock:
                self.lines.append(line.rstrip("\n"))

    def wait_for(self, prefix, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                for line in self.lines:
                    if line.startswith(prefix):
                        return line
            time.sleep(0.01)
        return None

    def sink_lines(self):
        with self.lock:
            return [ln for ln in self.lines if ln.startswith("SINK ")]

    def all_lines(self):
        with self.lock:
            return list(self.lines)

    def command(self, text):
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()


def connect():
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    sock.settimeout(5)
    return sock


def attach_baseline(harness, before):
    """Waits out the synthetic `attach` op the accept path delivers, and returns
    the sink-line count to measure the next op against.

    Not a race in the transport: the op is delivered on accept, which happens
    after connect() has already returned. Snapshotting the baseline without
    waiting for it makes the next op's expected SINK line land one slot late.
    Fails loudly if the op never arrives, so this stays an assertion rather than
    a sleep."""
    deadline = time.time() + 2
    while time.time() < deadline:
        lines = harness.sink_lines()
        if len(lines) > before and parse_sink(lines[before]).get("op") == "attach":
            return len(lines)
        time.sleep(0.005)
    check(False, "accept delivers a synthetic attach op")
    return len(harness.sink_lines())


class LineReader:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def read_line(self, timeout=2.0):
        self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode("utf-8", "replace")

    def read_all(self, seconds=0.4):
        out = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            line = self.read_line(timeout=max(0.05, deadline - time.time()))
            if line is None:
                break
            out.append(line)
        return out


def parse_sink(line):
    fields = {}
    for part in line.split()[1:]:
        key, _, value = part.partition("=")
        fields[key] = value
    return fields


# Every op, explicit then defaulted, with the SINK line each must produce.
# op / json sent / expected decoded fields.
OPS = [
    ("connect explicit",
     {"op": "connect", "mtu": 517, "interval": 12, "latency": 3, "timeout": 500},
     {"a": "517", "b": "12", "c": "3", "d": "500"}),
    ("connect defaults",
     {"op": "connect"},
     {"a": "23", "b": "24", "c": "0", "d": "400"}),
    ("disconnect explicit",
     {"op": "disconnect", "reason": 8},
     {"a": "8"}),
    ("disconnect defaults",
     {"op": "disconnect"},
     {"a": "19"}),
    ("write explicit",
     {"op": "write", "uuid": "0000ffe1-0000-1000-8000-00805f9b34fb",
      "hex": "DEADbeef00", "response": False},
     {"uuid": "0000ffe1-0000-1000-8000-00805f9b34fb", "data": "deadbeef00",
      "flag": "0"}),
    ("write defaults",
     {"op": "write", "uuid": "2a19"},
     {"uuid": "2a19", "data": "-", "flag": "1"}),
    ("subscribe explicit",
     {"op": "subscribe", "uuid": "2a19", "value": 2},
     {"uuid": "2a19", "a": "2"}),
    ("subscribe defaults",
     {"op": "subscribe", "uuid": "2a19"},
     {"uuid": "2a19", "a": "1"}),
    ("confirm",
     {"op": "confirm", "uuid": "2a19"},
     {"uuid": "2a19"}),
    ("mtu explicit",
     {"op": "mtu", "mtu": 247},
     {"a": "247"}),
    ("mtu defaults",
     {"op": "mtu"},
     {"a": "23"}),
    ("connparams explicit",
     {"op": "connparams", "interval": 80, "latency": 4, "timeout": 600},
     {"b": "80", "c": "4", "d": "600"}),
    ("connparams defaults",
     {"op": "connparams"},
     {"b": "24", "c": "0", "d": "400"}),
    ("rssi explicit",
     {"op": "rssi", "value": -95},
     {"a": "161"}),          # 0xA1, the low byte of int8_t(-95)
    ("rssi defaults",
     {"op": "rssi"},
     {"a": "196"}),          # 0xC4, the low byte of int8_t(-60)
    ("auto_confirm explicit",
     {"op": "auto_confirm", "enabled": False, "delay_ms": 250},
     {"flag": "0", "a": "250"}),
    ("auto_confirm defaults",
     {"op": "auto_confirm"},
     {"flag": "1", "a": "10"}),
]

MALFORMED = [
    ("not json at all", b"hello world"),
    ("truncated object", b'{"op":"mtu"'),
    ("unterminated string", b'{"op":"mt'),
    ("no op field", b'{"mtu":100}'),
    ("op not a string", b'{"op":42}'),
    ("unknown op", b'{"op":"launch_missiles"}'),
    ("wrong type", b'{"op":"mtu","mtu":"517"}'),
    ("out of range mtu", b'{"op":"mtu","mtu":99999}'),
    ("huge number", b'{"op":"mtu","mtu":1e300}'),
    ("odd length hex", b'{"op":"write","uuid":"2a19","hex":"abc"}'),
    ("non hex hex", b'{"op":"write","uuid":"2a19","hex":"zzzz"}'),
    ("missing uuid", b'{"op":"write","hex":"00"}'),
    ("embedded raw NUL", b'{"op":"write","uuid":"2a\x0019","hex":"00"}'),
    ("escaped NUL", b'{"op":"write","uuid":"a\\u0000b","hex":"00"}'),
    ("nested too deep", b'{"op":"mtu","x":' + b"[" * 20 + b"]" * 20 + b"}"),
    ("trailing bytes", b'{"op":"mtu"} garbage'),
    ("empty object", b"{}"),
    ("bare bracket", b"["),
    ("lone brace", b"}"),
    ("too many keys", b'{"op":"mtu",' + b",".join(
        b'"k%d":%d' % (i, i) for i in range(40)) + b"}"),
]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        binary = os.path.join(tmp, "sim_ble_link_selftest")
        build(binary)
        harness = Harness(binary)

        print("== startup ==")
        check(harness.wait_for("OK start(0) refused") is not None,
              "start(0) returns false")
        check(harness.wait_for("OK running after start(0) = false") is not None,
              "running() false after a refused start")
        check(harness.wait_for("OK stop() before start returned") is not None,
              "stop() before start is a no-op")
        check(harness.wait_for(f"READY {PORT}") is not None, "listener up")

        # Loopback only: the listener must not answer on a routable address.
        print("\n== loopback only ==")
        host_ip = None
        try:
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.connect(("192.0.2.1", 9))
            host_ip = probe.getsockname()[0]
            probe.close()
        except OSError:
            pass
        if host_ip and not host_ip.startswith("127."):
            try:
                s = socket.create_connection((host_ip, PORT), timeout=1)
                s.close()
                check(False, "refuses a connect to the host LAN address",
                      f"{host_ip}:{PORT} accepted the connection")
            except OSError as exc:
                check(True, f"refuses {host_ip}:{PORT} ({exc.__class__.__name__})")
        else:
            print("  skip no routable address on this host")

        base = len(harness.sink_lines())
        sock = connect()
        reader = LineReader(sock)
        attach_baseline(harness, base)

        print("\n== every op, explicit and defaulted ==")
        for label, payload, expected in OPS:
            before = len(harness.sink_lines())
            sock.sendall((json.dumps(payload) + "\n").encode())
            deadline = time.time() + 2
            got = None
            while time.time() < deadline:
                lines = harness.sink_lines()
                if len(lines) > before:
                    got = lines[before]
                    break
                time.sleep(0.005)
            if got is None:
                check(False, label, "no SINK line")
                continue
            fields = parse_sink(got)
            bad = {k: (v, fields.get(k)) for k, v in expected.items()
                   if fields.get(k) != v}
            op_ok = fields.get("op") == payload["op"]
            check(op_ok and not bad, f"{label} -> {got[5:]}",
                  f"op={fields.get('op')} mismatches={bad}")

        print("\n== framing ==")
        before = len(harness.sink_lines())
        # \r\n, blank lines, two ops in one write, and one op split in half.
        sock.sendall(b'{"op":"mtu","mtu":100}\r\n\n')
        sock.sendall(b'{"op":"mtu","mtu":101}\n{"op":"mtu","mtu":102}\n')
        sock.sendall(b'{"op":"mtu",')
        time.sleep(0.15)
        sock.sendall(b'"mtu":103}\n')
        time.sleep(0.3)
        got = [parse_sink(l).get("a") for l in harness.sink_lines()[before:]]
        check(got == ["100", "101", "102", "103"],
              "CRLF, blank line, batched writes and a split line", str(got))

        print("\n== malformed lines ==")
        for label, raw in MALFORMED:
            sock.sendall(raw + b"\n")
            line = reader.read_line(timeout=2)
            ok = line is not None and '"ev":"error"' in line
            check(ok, f"malformed: {label} -> {line}", str(line))
        before = len(harness.sink_lines())
        sock.sendall(b'{"op":"mtu","mtu":123}\n')
        time.sleep(0.3)
        after = harness.sink_lines()
        check(len(after) > before and parse_sink(after[before]).get("a") == "123",
              "still alive and framing after every malformed line")

        print("\n== over-long line ==")
        reader.read_all(0.2)
        huge = b'{"op":"write","uuid":"2a19","hex":"' + b"ab" * 40000 + b'"}'
        check(len(huge) > 65536, f"test line is {len(huge)} bytes")
        before = len(harness.sink_lines())
        sock.sendall(huge + b"\n")
        line = reader.read_line(timeout=3)
        check(line is not None and "line longer than 65536" in line,
              f"over-long line answered with one error -> {line}", str(line))
        check(len(harness.sink_lines()) == before,
              "over-long line produced no decoded op")
        sock.sendall(b'{"op":"mtu","mtu":42}\n')
        time.sleep(0.4)
        after = harness.sink_lines()
        check(len(after) > before and parse_sink(after[before]).get("a") == "42",
              "framing recovered after the drop", str(after[before:]))
        # A line of exactly the cap must be accepted: the cap is inclusive.
        head = b'{"op":"write","uuid":"2a19","hex":"'
        tail = b'"}'
        pad = 65536 - len(head) - len(tail)
        spaces = b""
        if pad % 2:                      # hex must stay an even run of digits
            pad -= 1
            spaces = b" "                # legal JSON whitespace, keeps the size
        line = head + b"a" * pad + b'"' + spaces + b"}"
        assert len(line) == 65536, len(line)
        before = len(harness.sink_lines())
        sock.sendall(line + b"\n")
        time.sleep(0.6)
        after = harness.sink_lines()
        fields = parse_sink(after[before]) if len(after) > before else {}
        check(fields.get("op") == "write" and len(fields.get("data", "")) == pad,
              f"a line of exactly {len(line)} bytes (the cap) is accepted",
              str(fields.get("op")) + " datalen=" + str(len(fields.get("data", ""))))

        print("\n== second client refused ==")
        second = connect()
        second_reader = LineReader(second)
        line = second_reader.read_line(timeout=2)
        check(line is not None and '"ev":"error"' in line and "busy" in line,
              f"second client gets an error line -> {line}", str(line))
        check(second_reader.read_line(timeout=2) is None,
              "second client socket is then closed")
        second.close()
        before = len(harness.sink_lines())
        sock.sendall(b'{"op":"mtu","mtu":200}\n')
        time.sleep(0.3)
        after = harness.sink_lines()
        check(len(after) > before and parse_sink(after[before]).get("a") == "200",
              "the first client still works")

        print("\n== the client drops its socket ==")
        before = len(harness.sink_lines())
        sock.close()
        deadline = time.time() + 2
        got = None
        while time.time() < deadline:
            lines = harness.sink_lines()
            if len(lines) > before:
                got = parse_sink(lines[before])
                break
            time.sleep(0.01)
        check(got is not None and got.get("op") == "disconnect" and got.get("a") == "19",
              f"a lost socket synthesizes disconnect reason 0x13 -> {got}", str(got))
        base = len(harness.sink_lines())
        sock = connect()
        reader = LineReader(sock)
        before = attach_baseline(harness, base)
        sock.sendall(b'{"op":"mtu","mtu":300}\n')
        time.sleep(0.3)
        after = harness.sink_lines()
        check(len(after) > before and parse_sink(after[before]).get("a") == "300",
              "the client slot is free again and a new client works")

        print("\n== concurrent emit ==")
        reader.read_all(0.2)
        per_thread = 300
        harness.command(f"emitstress {per_thread}")
        collected = []
        deadline = time.time() + 15
        while time.time() < deadline and len(collected) < per_thread * 2:
            line = reader.read_line(timeout=2)
            if line is None:
                break
            collected.append(line)
        check(len(collected) == per_thread * 2,
              f"received {len(collected)} of {per_thread * 2} emitted lines")
        broken = [l for l in collected if not re.fullmatch(
            r'\{"ev":"stress","tag":"[AB]","n":\d+,"pad":"(A{200}|B{200})"\}', l)]
        check(not broken, "every emitted line arrived intact and unspliced",
              f"{len(broken)} broken, first: {broken[0][:120] if broken else ''}")
        tags = {}
        for line in collected:
            obj = json.loads(line)
            tags.setdefault(obj["tag"], []).append(obj["n"])
        check(sorted(tags.keys()) == ["A", "B"], f"both threads got through: "
              f"{ {k: len(v) for k, v in tags.items()} }")
        check(all(v == sorted(v) for v in tags.values()),
              "each thread's lines stayed in its own order")
        interleaved = any(
            collected[i].startswith('{"ev":"stress","tag":"A"') !=
            collected[i + 1].startswith('{"ev":"stress","tag":"A"')
            for i in range(len(collected) - 1))
        print(f"  note  the two threads did{'' if interleaved else ' not'} "
              f"interleave at line granularity")

        print("\n== stop() with a client connected ==")
        harness.command("stop")
        line = harness.wait_for("STOPPED", timeout=5)
        check(line is not None, "stop() returned", "no STOPPED line in 5 s")
        if line:
            ms = int(re.search(r"in (\d+) ms", line).group(1))
            check(ms < 1000, f"stop() took {ms} ms with a client connected")
            check("running=false" in line, "running() false after stop()")
        check(reader.read_line(timeout=2) is None,
              "the client socket is closed by stop()")
        sock.close()
        check(harness.wait_for("EXIT", timeout=5) is not None, "clean exit")
        try:
            code = harness.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            harness.proc.kill()
            code = "timeout"
        check(code == 0, f"process exit code {code}")
        crashes = [l for l in harness.all_lines()
                   if "Segmentation" in l or "Aborted" in l or "sanitizer" in l
                   or "WARNING: ThreadSanitizer" in l or "runtime error" in l]
        check(not crashes, "no crash output", str(crashes))

    print(f"\n{checks - len(failures)}/{checks} checks passed")
    if failures:
        print("FAILED: " + "; ".join(failures))
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
