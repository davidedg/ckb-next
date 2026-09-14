#!/usr/bin/env python3
# Protocol-level test harness for the ckb-next "hwsensor" animation.
#
# Speaks the ckb-next animation stdin/stdout protocol directly to a built
# animation binary (see src/libs/ckb-next/include/ckb-next/animation.h) --
# no GUI/Qt involved. Used because this repo has no unit test framework
# (see CLAUDE.md); this script is the regression check for hwsensor.
#
# Run from the repo root after building, e.g.:
#   cmake --build build --target hwsensor heat
#   python3 scripts/hwsensor_harness.py

import argparse
import os
import queue
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import unquote

TIMEOUT = object()  # sentinel returned by read_line() on timeout
SANITIZER_WARNINGS = []  # (binary, stderr) pairs flagged by any session, checked at the end


def ckb_percent_encode(s):
    """Mirrors printurl() in animation.h exactly (same escape set, same
    uppercase-hex form) -- ckb_getline() only tokenizes on whitespace, so
    any value containing a space (e.g. a multi-stop gradient string) MUST
    be percent-encoded on the wire or it gets silently truncated."""
    out = []
    for ch in s:
        b = ord(ch)
        if b <= 0x2C or ch == "/" or (0x3A <= b <= 0x40) or ch in "[]" or b >= 0x7F:
            out.append(f"%{b:02X}")
        else:
            out.append(ch)
    return "".join(out)


class AnimSession:
    """Drives one `<binary> --ckb-run` process via the line-based protocol."""

    def __init__(self, binary, env=None):
        self.binary = binary
        self.proc = subprocess.Popen(
            [binary, "--ckb-run"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=env,
        )
        self.q = queue.Queue()
        self.reader = threading.Thread(target=self._read_stdout, daemon=True)
        self.reader.start()
        self.stderr_lines = []
        self.stderr_reader = threading.Thread(target=self._read_stderr, daemon=True)
        self.stderr_reader.start()

    def _read_stdout(self):
        try:
            for line in self.proc.stdout:
                self.q.put(line.rstrip("\n"))
        except ValueError:
            pass
        self.q.put(None)  # EOF

    def _read_stderr(self):
        # Drained continuously (not just at close()) so a large sanitizer
        # report can't fill the pipe and stall the child.
        try:
            for line in self.proc.stderr:
                self.stderr_lines.append(line.rstrip("\n"))
        except ValueError:
            pass

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def read_line(self, timeout):
        try:
            line = self.q.get(timeout=timeout)
        except queue.Empty:
            return TIMEOUT
        return line

    def read_until(self, predicate, timeout):
        """Reads lines until predicate(line) is true; returns the list of
        lines read (including the matching one), or None on timeout/EOF."""
        deadline = time.monotonic() + timeout
        lines = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            line = self.read_line(remaining)
            if line is TIMEOUT or line is None:
                return None
            lines.append(line)
            if predicate(line):
                return lines

    def send_keymap(self, keys):
        """keys: list of (name, x, y)."""
        self.send("begin keymap")
        self.send(f"keycount {len(keys)}")
        for name, x, y in keys:
            self.send(f"key {name} {x},{y}")
        self.send("end keymap")

    def send_params(self, params, begin_run=False):
        self.send("begin params")
        for k, v in params.items():
            self.send(f"param {k} {ckb_percent_encode(v)}")
        self.send("end params")
        if begin_run:
            self.send("begin run")
            lines = self.read_until(lambda l: l == "begin run", timeout=2.0)
            if lines is None:
                raise AssertionError("did not see 'begin run' echoed back")

    def request_frame(self, timeout):
        """Sends 'frame', waits for the matching 'end frame'. Returns
        (elapsed_seconds, {keyname: 'AARRGGBB'})."""
        start = time.monotonic()
        self.send("frame")
        lines = self.read_until(lambda l: l == "end frame", timeout=timeout)
        elapsed = time.monotonic() - start
        if lines is None:
            raise AssertionError(f"no 'end frame' within {timeout}s")
        colors = {}
        for line in lines:
            if line.startswith("argb "):
                _, name, hexval = line.split(" ", 2)
                colors[name] = hexval
        return elapsed, colors

    def send_time(self, delta):
        self.send(f"time {delta}")

    def close(self):
        try:
            self.send("end run")
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()
            try:
                self.proc.wait(timeout=2)
            except Exception:
                pass
        self.stderr_reader.join(timeout=1)
        combined = "\n".join(self.stderr_lines)
        if "ThreadSanitizer" in combined or "AddressSanitizer" in combined or "UndefinedBehaviorSanitizer" in combined:
            SANITIZER_WARNINGS.append((self.binary, combined))


def run_ckb_info(binary, timeout, env=None):
    """Runs `<binary> --ckb-info`, returns (elapsed_seconds, stdout_text)."""
    start = time.monotonic()
    result = subprocess.run(
        [binary, "--ckb-info"],
        capture_output=True,
        text=True,
        timeout=timeout,
        env=env,
    )
    elapsed = time.monotonic() - start
    return elapsed, result.stdout


def parse_info(text):
    """Very small parser, enough for assertions -- not the real GUI parser."""
    info = {"params": [], "listitems": [], "presets": []}
    for line in text.splitlines():
        parts = line.split(" ", 1)
        head = parts[0]
        rest = parts[1] if len(parts) > 1 else ""
        if head == "param":
            info["params"].append(rest)
        elif head == "listitem":
            info["listitems"].append(rest)
        elif head == "preset":
            info["presets"].append(rest)
        else:
            info[head] = rest
    return info


def listitem_ids(info):
    """Decodes 'sensor <id>=<label> <group>' listitem lines (id/label/group
    are percent-encoded on the wire, see printurl() in animation.h) into a
    plain list of ids."""
    ids = []
    for li in info["listitems"]:
        rest = li.split(" ", 1)[1] if " " in li else li
        encoded_id = rest.split(" ", 1)[0].split("=", 1)[0]
        ids.append(unquote(encoded_id))
    return ids


def listitem_groups(info):
    """Decodes the trailing <group> token of each 'sensor <id>=<label>
    <group>' listitem line."""
    groups = []
    for li in info["listitems"]:
        rest = li.split(" ", 1)[1] if " " in li else li
        pieces = rest.split(" ", 1)
        groups.append(unquote(pieces[1]) if len(pieces) > 1 else "")
    return groups


def run_ckb_query(binary, name, value, timeout, env=None):
    """Runs `<binary> --ckb-query <name> <value>`, returns (elapsed_seconds,
    stdout_text_stripped)."""
    start = time.monotonic()
    result = subprocess.run(
        [binary, "--ckb-query", name, value],
        capture_output=True,
        text=True,
        timeout=timeout,
        env=env,
    )
    elapsed = time.monotonic() - start
    return elapsed, result.stdout.strip()


class Result:
    def __init__(self):
        self.failures = []

    def check(self, name, condition, detail=""):
        status = "PASS" if condition else "FAIL"
        print(f"[{status}] {name}" + (f" -- {detail}" if detail and not condition else ""))
        if not condition:
            self.failures.append(name)


def test_basic_protocol(result, binary, label):
    """Minimal keymap -> params -> frame -> end run cycle. Proves the
    harness itself talks the protocol correctly (run first against a known-
    good animation like `heat`), then reused against `hwsensor`."""
    elapsed, info_text = run_ckb_info(binary, timeout=1.5)
    result.check(f"{label}: --ckb-info returns within 1s", elapsed < 1.0, f"took {elapsed:.3f}s")
    info = parse_info(info_text)
    result.check(f"{label}: --ckb-info has a guid", bool(info.get("guid")))
    result.check(f"{label}: --ckb-info has at least one preset", len(info["presets"]) > 0)

    session = AnimSession(binary)
    try:
        session.send_keymap([("A", 0, 0), ("B", 1, 0)])
        session.send_params({}, begin_run=True)
        elapsed, colors = session.request_frame(timeout=2.0)
        result.check(f"{label}: frame responds within 2s", elapsed < 2.0, f"took {elapsed:.3f}s")
        result.check(f"{label}: frame includes both keys", set(colors) == {"A", "B"}, f"got {colors}")
        for name, hexval in colors.items():
            result.check(
                f"{label}: {name} color is well-formed argb hex",
                len(hexval) == 8 and all(c in "0123456789abcdefABCDEF" for c in hexval),
                hexval,
            )
    finally:
        session.close()


def test_hwsensor_list_param(result, binary):
    """hwsensor-specific: the new 'list' param type must be present and
    carry the stub's fake sensor choice."""
    elapsed, info_text = run_ckb_info(binary, timeout=1.5)
    info = parse_info(info_text)
    list_params = [p for p in info["params"] if p.startswith("list ")]
    result.check("hwsensor: declares a 'list' param", len(list_params) == 1, info["params"])
    result.check(
        "hwsensor: emits at least one listitem",
        len(info["listitems"]) >= 1,
        info["listitems"],
    )
    if list_params:
        result.check(
            "hwsensor: listitem is scoped to the 'sensor' param",
            all(li.startswith("sensor ") for li in info["listitems"]),
            info["listitems"],
        )
    groups = listitem_groups(info)
    known_groups = {"System (hwmon)", "OpenLinkHub"}
    result.check(
        "hwsensor: every listitem carries a known non-empty group",
        len(groups) > 0 and all(g in known_groups for g in groups),
        groups,
    )
    result.check(
        "hwsensor: --ckb-info advertises query support",
        info.get("query") == "on",
        info.get("query"),
    )


def test_query_mode(result, binary):
    """--ckb-query <param> <value> is the one-shot mode the GUI uses to show
    a live reading next to the sensor dropdown (see CKB_ENABLE_QUERY in
    animation.h) -- must never call sensor_discover_all(), so it stays fast
    even when olh is unreachable, and must never error out with a non-error
    exit status."""
    elapsed, out = run_ckb_query(binary, "sensor", "fake:42", timeout=1.5)
    result.check("query: fake:42 returns the deterministic value", out == "value 42.0", out)
    result.check("query: fake: is effectively instant", elapsed < 0.2, f"took {elapsed:.3f}s")

    _, out = run_ckb_query(binary, "sensor", "bogus:whatever", timeout=1.5)
    result.check("query: unknown prefix reports error", out == "error", out)

    _, out = run_ckb_query(binary, "not_sensor", "fake:42", timeout=1.5)
    result.check("query: unknown param name reports error", out == "error", out)

    _, out = run_ckb_query(binary, "sensor", "hwmon:nvme/temp1", timeout=1.5)
    result.check(
        "query: real hwmon sensor returns a parseable value",
        out.startswith("value ") and _is_float(out[len("value "):]),
        out,
    )

    # Query must stay within its own budget even against a hanging olh peer --
    # this is the number that decides whether the GUI shows a value or times out.
    hang = HangServer()
    try:
        env_hang = dict(os.environ, HWSENSOR_OLH_URL=f"http://127.0.0.1:{hang.port}/api/devices/")
        elapsed, out = run_ckb_query(binary, "sensor", "olh:x/0/temperature", timeout=1.5, env=env_hang)
        result.check("query: hanging olh peer reports error", out == "error", out)
        result.check(
            "query: hanging olh peer still finishes well under the 1s GUI budget",
            elapsed < 0.7,
            f"took {elapsed:.3f}s",
        )
    finally:
        hang.close()


def _is_float(s):
    try:
        float(s)
        return True
    except ValueError:
        return False


DEFAULT_GRADIENT = "0:ff00ff00 50:ffffff00 100:ffff0000"


def wait_for_color(session, keyname, expected_hex, timeout, poll_interval=0.1):
    """Polls request_frame() until `keyname`'s color matches expected_hex
    (case-insensitive) or the overall timeout elapses. The poll thread
    inside hwsensor picks up a sensor-selection change via a condition
    variable (near-immediate), so this should converge quickly, not wait
    out the full 1s poll interval."""
    deadline = time.monotonic() + timeout
    last_seen = None
    while time.monotonic() < deadline:
        _, colors = session.request_frame(timeout=1.0)
        last_seen = colors.get(keyname)
        if last_seen and last_seen.lower() == expected_hex.lower():
            return True, last_seen
        time.sleep(poll_interval)
    return False, last_seen


def test_gradient_math(result, binary):
    """fake:<value> is a deterministic, I/O-free sensor -- exactly what the
    gradient mapping math should be tested against. With value_min=0 /
    value_max=100 and the default green/yellow/red gradient, fake:0,
    fake:50 and fake:100 must land exactly on the three gradient stops."""
    cases = [
        ("fake:0", "ff00ff00", "green stop (pos 0)"),
        ("fake:50", "ffffff00", "yellow stop (pos 50)"),
        ("fake:100", "ffff0000", "red stop (pos 100)"),
    ]
    for sensor_id, expected, desc in cases:
        session = AnimSession(binary)
        try:
            session.send_keymap([("A", 0, 0)])
            session.send_params(
                {
                    "sensor": sensor_id,
                    "color": DEFAULT_GRADIENT,
                    "value_min": "0",
                    "value_max": "100",
                },
                begin_run=True,
            )
            ok, seen = wait_for_color(session, "A", expected, timeout=3.0)
            result.check(
                f"hwsensor: {sensor_id} maps to {desc}",
                ok,
                f"expected {expected}, last saw {seen}",
            )
        finally:
            session.close()


def test_fallback_behavior(result, binary):
    """No I/O-bound backend exists yet, but the fallback path itself
    (unresolvable id, and no sensor selected at all) must already behave
    correctly: a configured fallback color, never a crash, never a stall."""
    session = AnimSession(binary)
    try:
        session.send_keymap([("A", 0, 0)])
        session.send_params(
            {"sensor": "bogus:nope", "fallback": "80112233"},
            begin_run=True,
        )
        ok, seen = wait_for_color(session, "A", "80112233", timeout=3.0)
        result.check(
            "hwsensor: unresolvable sensor id falls back to configured color",
            ok,
            f"expected 80112233, last saw {seen}",
        )
    finally:
        session.close()

    session = AnimSession(binary)
    try:
        session.send_keymap([("A", 0, 0)])
        session.send_params({}, begin_run=True)  # no 'sensor' param sent at all
        elapsed, colors = session.request_frame(timeout=2.0)
        result.check(
            "hwsensor: no sensor selected responds promptly (no blocking)",
            elapsed < 0.5,
            f"took {elapsed:.3f}s",
        )
        result.check(
            "hwsensor: no sensor selected uses the default fallback color",
            colors.get("A", "").lower() == "00000000",
            colors,
        )
    finally:
        session.close()


def test_frame_latency_under_load(result, binary):
    """The core 'no blocking I/O in the frame path' guarantee: even while
    rapidly re-selecting sensors (forcing the poll thread to repeatedly
    attempt reads), a 'frame' request must still be answered fast."""
    session = AnimSession(binary)
    try:
        session.send_keymap([("A", 0, 0)])
        session.send_params({"sensor": "fake:1"}, begin_run=True)
        worst = 0.0
        for i in range(20):
            session.send_params({"sensor": f"fake:{i}"})
            elapsed, _ = session.request_frame(timeout=1.0)
            worst = max(worst, elapsed)
        result.check(
            "hwsensor: frame latency stays low under rapid sensor changes",
            worst < 0.05,
            f"worst-case {worst * 1000:.1f}ms",
        )
    finally:
        session.close()


def test_hwmon_real_sensors(result, binary):
    """This VM's real /sys/class/hwmon: 'nvme' (has temp1_input, a garbage
    reading via USB passthrough -- exactly the case that rules out any
    plausibility filtering) and 'ACAD' (no temp*/fan* leaves at all)."""
    _, info_text = run_ckb_info(binary, timeout=1.5)
    info = parse_info(info_text)
    ids = listitem_ids(info)
    result.check(
        "hwmon: discovers the real nvme temp sensor on this VM",
        "hwmon:nvme/temp1" in ids,
        ids,
    )

    session = AnimSession(binary)
    try:
        session.send_keymap([("A", 0, 0)])
        session.send_params(
            {
                "sensor": "hwmon:nvme/temp1",
                "color": DEFAULT_GRADIENT,
                "value_min": "0",
                "value_max": "100",
            },
            begin_run=True,
        )
        # The real reading is nonsense (thousands of degrees, passthrough
        # artifact) -- exactly why no plausibility filter exists. It must
        # still resolve (not fall back) and saturate to the red end.
        ok, seen = wait_for_color(session, "A", "ffff0000", timeout=3.0)
        result.check(
            "hwmon: real (garbage) nvme reading resolves and saturates the gradient",
            ok,
            f"expected ffff0000 (saturated red), last saw {seen}",
        )
    finally:
        session.close()


def write_file(path, content, mode=0o644):
    with open(path, "w") as f:
        f.write(content)
    os.chmod(path, mode)


def build_hwmon_fixture(root):
    """chipA: unique name, one readable temp. hwmon1/hwmon2: both named
    'dup' -- must disambiguate to numbered ids. chipB: unique name, but its
    _input file is unreadable -- discovered (filename exists) yet must fail
    to resolve at read time, not crash."""
    chip_a = os.path.join(root, "hwmon0")
    os.makedirs(chip_a)
    write_file(os.path.join(chip_a, "name"), "chipA\n")
    write_file(os.path.join(chip_a, "temp1_input"), "42500\n")
    write_file(os.path.join(chip_a, "temp1_label"), "CPU\n")

    dup1 = os.path.join(root, "hwmon1")
    os.makedirs(dup1)
    write_file(os.path.join(dup1, "name"), "dup\n")
    write_file(os.path.join(dup1, "fan1_input"), "1234\n")

    dup2 = os.path.join(root, "hwmon2")
    os.makedirs(dup2)
    write_file(os.path.join(dup2, "name"), "dup\n")
    write_file(os.path.join(dup2, "temp1_input"), "30000\n")

    chip_b = os.path.join(root, "hwmon3")
    os.makedirs(chip_b)
    write_file(os.path.join(chip_b, "name"), "chipB\n")
    write_file(os.path.join(chip_b, "temp1_input"), "50000\n", mode=0o000)


def test_hwmon_fixture(result, binary):
    if os.geteuid() == 0:
        print("[SKIP] hwmon fixture: running as root, permission-denied case is not testable")
        return
    with tempfile.TemporaryDirectory() as root:
        build_hwmon_fixture(root)
        env = dict(os.environ, HWSENSOR_HWMON_ROOT=root)

        _, info_text = run_ckb_info(binary, timeout=1.5, env=env)
        info = parse_info(info_text)
        ids = listitem_ids(info)
        result.check(
            "hwmon fixture: unique chip name uses the readable id form",
            "hwmon:chipA/temp1" in ids,
            ids,
        )
        result.check(
            "hwmon fixture: colliding chip names are disambiguated to hwmonN, not the shared name",
            "hwmon:hwmon1/fan1" in ids and "hwmon:hwmon2/temp1" in ids and "hwmon:dup/fan1" not in ids,
            ids,
        )
        result.check(
            "hwmon fixture: permission-denied leaf is still discovered (listed, not hidden)",
            "hwmon:chipB/temp1" in ids,
            ids,
        )

        session = AnimSession(binary, env=env)
        try:
            session.send_keymap([("A", 0, 0)])
            session.send_params(
                {
                    "sensor": "hwmon:chipA/temp1",
                    "color": DEFAULT_GRADIENT,
                    "value_min": "0",
                    "value_max": "100",
                },
                begin_run=True,
            )
            # 42500 -> 42.5C -> pos 42.5 between green(0) and yellow(50) stops
            ok, seen = wait_for_color(session, "A", "ffd9ff00", timeout=3.0)
            result.check(
                "hwmon fixture: readable temp file resolves to the exact expected gradient color",
                ok,
                f"expected ffd9ff00, last saw {seen}",
            )
        finally:
            session.close()

        session = AnimSession(binary, env=env)
        try:
            session.send_keymap([("A", 0, 0)])
            session.send_params(
                {"sensor": "hwmon:chipB/temp1", "fallback": "80112233"},
                begin_run=True,
            )
            ok, seen = wait_for_color(session, "A", "80112233", timeout=3.0)
            result.check(
                "hwmon fixture: permission-denied leaf falls back cleanly (no crash)",
                ok,
                f"expected 80112233, last saw {seen}",
            )
        finally:
            session.close()


OLH_FIXTURE_PAYLOAD = """
{
  "code": 200, "status": 0,
  "devices": {
    "TESTSERIAL01": {
      "GetDevice": {
        "devices": {
          "0": {"channelId": 0, "deviceId": "AIO-0", "name": "Test AIO",
                "rpm": 2100, "temperature": 41.5, "temperatureString": "41.5 C",
                "HasSpeed": true, "HasTemps": true},
          "1": {"channelId": 1, "deviceId": "Fan-1", "name": "Fan 1",
                "rpm": 980, "temperature": 0, "temperatureString": "",
                "HasSpeed": true, "HasTemps": false}
        }
      }
    }
  }
}
"""


class HangServer:
    """Accepts TCP connections but never sends a byte -- used to exercise
    the recv-timeout path (as opposed to connection-refused)."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(5)
        self.port = self.sock.getsockname()[1]
        self._stop = False
        self._conns = []
        self.thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.thread.start()

    def _accept_loop(self):
        self.sock.settimeout(0.2)
        while not self._stop:
            try:
                conn, _ = self.sock.accept()
                self._conns.append(conn)  # held open, never written to
            except socket.timeout:
                continue
            except OSError:
                break

    def close(self):
        self._stop = True
        self.thread.join(timeout=1)
        for c in self._conns:
            c.close()
        self.sock.close()


def test_olh_fixture(result, binary):
    with tempfile.TemporaryDirectory() as root:
        valid_path = os.path.join(root, "valid.json")
        write_file(valid_path, OLH_FIXTURE_PAYLOAD)
        env = dict(os.environ, HWSENSOR_OLH_URL=f"file://{valid_path}")

        _, info_text = run_ckb_info(binary, timeout=1.5, env=env)
        info = parse_info(info_text)
        ids = listitem_ids(info)
        result.check(
            "olh fixture: AIO channel exposes both temperature and rpm",
            "olh:TESTSERIAL01/0/temperature" in ids and "olh:TESTSERIAL01/0/rpm" in ids,
            ids,
        )
        result.check(
            "olh fixture: fan-only channel (HasTemps=false) is not offered as a temperature sensor",
            "olh:TESTSERIAL01/1/temperature" not in ids and "olh:TESTSERIAL01/1/rpm" in ids,
            ids,
        )

        session = AnimSession(binary, env=env)
        try:
            session.send_keymap([("A", 0, 0)])
            session.send_params(
                {
                    "sensor": "olh:TESTSERIAL01/0/temperature",
                    "color": DEFAULT_GRADIENT,
                    "value_min": "0",
                    "value_max": "100",
                },
                begin_run=True,
            )
            # 41.5 -> pos 41.5, between green(0)/yellow(50): r = round(255*41.5/50) = 212 = 0xd4
            ok, seen = wait_for_color(session, "A", "ffd4ff00", timeout=3.0)
            result.check(
                "olh fixture: reads the exact value from a frozen JSON payload",
                ok,
                f"expected ffd4ff00, last saw {seen}",
            )
        finally:
            session.close()

        malformed_path = os.path.join(root, "malformed.json")
        write_file(malformed_path, "{ this is not valid json ]")
        env_bad = dict(os.environ, HWSENSOR_OLH_URL=f"file://{malformed_path}")

        elapsed, info_text = run_ckb_info(binary, timeout=1.5, env=env_bad)
        info = parse_info(info_text)
        ids = listitem_ids(info)
        result.check(
            "olh fixture: malformed JSON contributes zero olh: entries, doesn't crash discovery",
            not any(i.startswith("olh:") for i in ids),
            ids,
        )

        session = AnimSession(binary, env=env_bad)
        try:
            session.send_keymap([("A", 0, 0)])
            session.send_params(
                {"sensor": "olh:TESTSERIAL01/0/temperature", "fallback": "80112233"},
                begin_run=True,
            )
            ok, seen = wait_for_color(session, "A", "80112233", timeout=3.0)
            result.check(
                "olh fixture: malformed JSON at read time falls back cleanly",
                ok,
                f"expected 80112233, last saw {seen}",
            )
        finally:
            session.close()


def test_olh_network_failures(result, binary):
    # Nothing listening on this port -- connection refused, should fail fast.
    env_refused = dict(os.environ, HWSENSOR_OLH_URL="http://127.0.0.1:1/api/devices/")
    elapsed, info_text = run_ckb_info(binary, timeout=1.5, env=env_refused)
    result.check(
        "olh network: connection-refused discovery still finishes well under the 1s GUI budget",
        elapsed < 0.7,
        f"took {elapsed:.3f}s",
    )
    info = parse_info(info_text)
    result.check(
        "olh network: connection-refused contributes zero olh: entries",
        not any(i.startswith("olh:") for i in listitem_ids(info)),
        info["listitems"],
    )

    # Accepts the connection but never answers -- exercises the recv timeout.
    hang = HangServer()
    try:
        env_hang = dict(os.environ, HWSENSOR_OLH_URL=f"http://127.0.0.1:{hang.port}/api/devices/")
        elapsed, _ = run_ckb_info(binary, timeout=1.5, env=env_hang)
        result.check(
            "olh network: a hanging peer still finishes discovery well under the 1s GUI budget",
            elapsed < 0.7,
            f"took {elapsed:.3f}s",
        )

        session = AnimSession(binary, env=env_hang)
        try:
            session.send_keymap([("A", 0, 0)])
            session.send_params(
                {"sensor": "olh:whatever/0/temperature", "fallback": "80112233"},
                begin_run=True,
            )
            elapsed, colors = session.request_frame(timeout=1.0)
            result.check(
                "olh network: frame responds promptly even while the poll thread is stuck on a hanging read",
                elapsed < 0.05,
                f"took {elapsed * 1000:.1f}ms",
            )
            ok, seen = wait_for_color(session, "A", "80112233", timeout=3.0)
            result.check(
                "olh network: hanging peer falls back cleanly once the recv timeout fires",
                ok,
                f"expected 80112233, last saw {seen}",
            )
        finally:
            session.close()
    finally:
        hang.close()


def wait_for_condition(predicate, timeout, poll_interval=0.2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(poll_interval)
    return False


def openlinkhub_reachable():
    try:
        with socket.create_connection(("127.0.0.1", 27003), timeout=0.5):
            return True
    except OSError:
        return False


def test_olh_live_service(result, binary):
    """Uses the real, already-running OpenLinkHub user service -- stops and
    restarts it to exercise the connection-refused/recovery path against
    the actual H150i, not just a fixture. Restores service state before
    returning even if an assertion fails."""
    if not openlinkhub_reachable():
        print("[SKIP] olh live service: OpenLinkHub is not reachable on 127.0.0.1:27003")
        return

    _, info_text = run_ckb_info(binary, timeout=1.5)
    info = parse_info(info_text)
    ids = listitem_ids(info)
    live_temp_ids = [i for i in ids if i.startswith("olh:") and i.endswith("/temperature")]
    result.check("olh live: discovers at least one real liquid/AIO temperature sensor", len(live_temp_ids) >= 1, ids)

    if live_temp_ids:
        session = AnimSession(binary)
        try:
            session.send_keymap([("A", 0, 0)])
            session.send_params(
                {"sensor": live_temp_ids[0], "color": DEFAULT_GRADIENT, "value_min": "0", "value_max": "100"},
                begin_run=True,
            )
            time.sleep(1.5)
            _, colors = session.request_frame(timeout=2.0)
            result.check(
                "olh live: reading the real sensor does not fall back to black/unset",
                colors.get("A", "").lower() not in ("", "00000000"),
                colors,
            )
        finally:
            session.close()

    stopped = subprocess.run(["systemctl", "--user", "stop", "OpenLinkHub"], capture_output=True).returncode == 0
    try:
        if not stopped:
            print("[SKIP] olh live service: could not stop the systemd --user unit, skipping stop/restart check")
            return
        wait_for_condition(lambda: not openlinkhub_reachable(), timeout=5.0)
        result.check("olh live: service is actually stopped before continuing", not openlinkhub_reachable())

        session = AnimSession(binary)
        try:
            session.send_keymap([("A", 0, 0)])
            session.send_params(
                {"sensor": live_temp_ids[0] if live_temp_ids else "olh:x/0/temperature", "fallback": "80112233"},
                begin_run=True,
            )
            ok, seen = wait_for_color(session, "A", "80112233", timeout=3.0)
            result.check("olh live: stopping the real service falls back cleanly, no crash", ok, seen)
        finally:
            session.close()
    finally:
        subprocess.run(["systemctl", "--user", "start", "OpenLinkHub"], capture_output=True)
        recovered = wait_for_condition(openlinkhub_reachable, timeout=15.0)
        result.check("olh live: service was restarted successfully after the test", recovered)

    if recovered and live_temp_ids:
        session = AnimSession(binary)
        try:
            session.send_keymap([("A", 0, 0)])
            session.send_params(
                {"sensor": live_temp_ids[0], "color": DEFAULT_GRADIENT, "value_min": "0", "value_max": "100"},
                begin_run=True,
            )
            # allow a couple of poll cycles for the freshly-restarted service to be reachable again
            deadline = time.monotonic() + 6.0
            colors = {}
            while time.monotonic() < deadline:
                _, colors = session.request_frame(timeout=2.0)
                if colors.get("A", "").lower() not in ("", "00000000"):
                    break
                time.sleep(0.5)
            result.check(
                "olh live: reading resumes after the service comes back up",
                colors.get("A", "").lower() not in ("", "00000000"),
                colors,
            )
        finally:
            session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bindir", default="build/bin", help="directory with built animation binaries")
    args = parser.parse_args()

    result = Result()

    heat = f"{args.bindir}/heat"
    hwsensor = f"{args.bindir}/hwsensor"

    print("== Validating the harness itself against 'heat' ==")
    test_basic_protocol(result, heat, "heat")

    print("\n== Testing 'hwsensor' ==")
    test_basic_protocol(result, hwsensor, "hwsensor")
    test_hwsensor_list_param(result, hwsensor)
    test_query_mode(result, hwsensor)
    test_gradient_math(result, hwsensor)
    test_fallback_behavior(result, hwsensor)
    test_frame_latency_under_load(result, hwsensor)
    test_hwmon_real_sensors(result, hwsensor)
    test_hwmon_fixture(result, hwsensor)
    test_olh_fixture(result, hwsensor)
    test_olh_network_failures(result, hwsensor)
    test_olh_live_service(result, hwsensor)

    result.check(
        "no sanitizer warnings on any session's stderr",
        len(SANITIZER_WARNINGS) == 0,
        f"{len(SANITIZER_WARNINGS)} session(s) flagged",
    )
    for binary, report in SANITIZER_WARNINGS:
        print(f"\n--- sanitizer report from {binary} ---\n{report}\n")

    print(f"\n{len(result.failures)} failure(s)" if result.failures else "\nAll checks passed")
    return 1 if result.failures else 0


if __name__ == "__main__":
    sys.exit(main())
