"""
ESP32 FreeRTOS Patient Monitor — Serial Test Runner
Automation flow:
  1. (Optional) Flash firmware via PlatformIO
  2. Open serial port
  3. Send test commands
  4. Collect logs
  5. Check pass/fail conditions
  6. Generate test report (CSV + JSON)
"""

import serial
import time
import json
import csv
import re
import sys
import argparse
import os
from datetime import datetime
from dataclasses import dataclass, field, asdict
from typing import Optional

# ─────────────────────────────────────────────
# Configuration
# ─────────────────────────────────────────────

DEFAULT_PORT     = "COM3"        # Change to /dev/ttyUSB0 on Linux/Mac
DEFAULT_BAUD     = 115200
DEFAULT_TIMEOUT  = 5             # seconds per command wait
LOG_DIR          = "reports"

# ─────────────────────────────────────────────
# Data Structures
# ─────────────────────────────────────────────

@dataclass
class TestResult:
    test_id:     str
    name:        str
    command:     str
    expected:    str
    passed:      bool
    actual:      str  = ""
    duration_s:  float = 0.0
    timestamp:   str  = ""
    notes:       str  = ""

@dataclass
class TestSession:
    session_id:    str
    port:          str
    baud:          int
    started_at:    str
    finished_at:   str = ""
    results:       list = field(default_factory=list)
    total:         int  = 0
    passed:        int  = 0
    failed:        int  = 0

# ─────────────────────────────────────────────
# Serial Helpers
# ─────────────────────────────────────────────

class SerialMonitor:
    def __init__(self, port: str, baud: int, timeout: float = 2.0):
        self.port    = port
        self.baud    = baud
        self.timeout = timeout
        self.ser: Optional[serial.Serial] = None
        self.log_lines: list[str] = []

    def connect(self) -> bool:
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
            time.sleep(2)          # let ESP32 boot
            self.ser.reset_input_buffer()
            print(f"[SERIAL] Connected to {self.port} @ {self.baud}")
            return True
        except serial.SerialException as e:
            print(f"[ERROR] Cannot open {self.port}: {e}")
            return False

    def disconnect(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("[SERIAL] Disconnected")

    def send(self, cmd: str):
        if self.ser and self.ser.is_open:
            self.ser.write((cmd + "\n").encode())
            print(f"[SEND]  > {cmd}")

    def read_until(self, pattern: str, timeout: float = DEFAULT_TIMEOUT) -> tuple[bool, str]:
        """Read lines until pattern found or timeout. Returns (found, all_lines_joined)."""
        collected = []
        deadline  = time.time() + timeout
        regex     = re.compile(pattern, re.IGNORECASE)

        while time.time() < deadline:
            if self.ser and self.ser.in_waiting:
                try:
                    line = self.ser.readline().decode("utf-8", errors="replace").strip()
                except Exception:
                    continue
                if line:
                    print(f"[RECV]    {line}")
                    collected.append(line)
                    self.log_lines.append(line)
                    if regex.search(line):
                        return True, "\n".join(collected)
            else:
                time.sleep(0.05)

        return False, "\n".join(collected)

    def drain(self, seconds: float = 1.0) -> list[str]:
        """Drain whatever is in the buffer for `seconds`."""
        lines    = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            if self.ser and self.ser.in_waiting:
                try:
                    line = self.ser.readline().decode("utf-8", errors="replace").strip()
                except Exception:
                    continue
                if line:
                    print(f"[RECV]    {line}")
                    lines.append(line)
                    self.log_lines.append(line)
            else:
                time.sleep(0.05)
        return lines

# ─────────────────────────────────────────────
# Test Definitions
# ─────────────────────────────────────────────

def build_test_suite() -> list[dict]:
    """
    Each entry:
      id       – unique test identifier
      name     – human-readable name
      command  – command to send (or "" for observe-only)
      expect   – regex pattern that must appear in output
      timeout  – seconds to wait for the pattern
      notes    – requirement tag
    """
    return [
        # ── Boot / Baseline ──────────────────────────────
        {
            "id": "T01", "name": "System Boot — RTOS tasks created",
            "command": "", "expect": r"All RTOS tasks created",
            "timeout": 10, "notes": "REQ:BOOT"
        },
        {
            "id": "T02", "name": "Logging task emits JSON",
            "command": "", "expect": r"\[LOG\].*heart_rate",
            "timeout": 6, "notes": "REQ:LOGGING"
        },
        {
            "id": "T03", "name": "Help command responds",
            "command": "help", "expect": r"Commands:",
            "timeout": 3, "notes": "REQ:CMD"
        },
        {
            "id": "T04", "name": "Diagnostics command responds",
            "command": "diag", "expect": r"=== Diagnostics ===",
            "timeout": 4, "notes": "REQ:DIAG"
        },

        # ── Fault Injection ──────────────────────────────
        {
            "id": "T05", "name": "Fault: analog ON — injects fault flag",
            "command": "fault analog on", "expect": r"Analog fault ON",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T06", "name": "Fault: analog OFF — clears fault flag",
            "command": "fault analog off", "expect": r"Analog fault OFF",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T07", "name": "Fault: I2C ON — blocks MAX30102",
            "command": "fault i2c on", "expect": r"I2C fault ON",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T08", "name": "Fault: I2C — MAX30102 read blocked in log",
            "command": "", "expect": r"Fault.*MAX30102 read blocked|MAX30102 data not updated",
            "timeout": 5, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T09", "name": "Fault: I2C OFF — restores sensor",
            "command": "fault i2c off", "expect": r"I2C fault OFF",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T10", "name": "Fault: API ON",
            "command": "fault api on", "expect": r"API fault ON",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T11", "name": "Fault: API — fallback decision used",
            "command": "", "expect": r"LOCAL P:|localFallback|API.*fault",
            "timeout": 6, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T12", "name": "Fault: API OFF",
            "command": "fault api off", "expect": r"API fault OFF",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T13", "name": "Fault: out-of-range ON",
            "command": "fault outofrange on", "expect": r"OOR fault ON",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T14", "name": "Fault: OOR forces invalid sensor value",
            "command": "", "expect": r"Analog OOR",
            "timeout": 4, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T15", "name": "Fault: out-of-range OFF",
            "command": "fault outofrange off", "expect": r"OOR fault OFF",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T16", "name": "Fault: stuck-touch ON",
            "command": "fault stuck on", "expect": r"Stuck ON",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T17", "name": "Fault: stuck makes emergencyTouch=true in log",
            "command": "", "expect": r"emergency_touch.*true|Emergency touch pressed",
            "timeout": 5, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T18", "name": "Fault: stuck OFF",
            "command": "fault stuck off", "expect": r"Stuck OFF",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },

        # ── Queue Overflow (Bonus) ────────────────────────
        {
            "id": "T19", "name": "Queue flood ON",
            "command": "fault qflood on", "expect": r"QFlood ON",
            "timeout": 3, "notes": "REQ:QUEUE_OVERFLOW"
        },
        {
            "id": "T20", "name": "Queue overflow — dropped/overwritten messages",
            "command": "", "expect": r"QueueFlood done|oldest result discarded|Dropped/overwritten",
            "timeout": 6, "notes": "REQ:QUEUE_OVERFLOW"
        },
        {
            "id": "T21", "name": "Queue flood OFF",
            "command": "fault qflood off", "expect": r"QFlood OFF",
            "timeout": 3, "notes": "REQ:QUEUE_OVERFLOW"
        },

        # ── Mutex / Timing ────────────────────────────────
        {
            "id": "T22", "name": "Fault: mutex hold too long ON",
            "command": "fault mutexlong on", "expect": r"MutexLong ON",
            "timeout": 3, "notes": "REQ:MUTEX"
        },
        {
            "id": "T23", "name": "Mutex held extra time visible in log",
            "command": "", "expect": r"holding riskMutex|MutexHoldTooLong",
            "timeout": 6, "notes": "REQ:MUTEX"
        },
        {
            "id": "T24", "name": "Fault: mutex hold OFF",
            "command": "fault mutexlong off", "expect": r"MutexLong OFF",
            "timeout": 3, "notes": "REQ:MUTEX"
        },

        # ── Stress Test ───────────────────────────────────
        {
            "id": "T25", "name": "Stress mode ON",
            "command": "stress on", "expect": r"Stress ON",
            "timeout": 3, "notes": "REQ:STRESS"
        },
        {
            "id": "T26", "name": "Diag under stress — WCET reported",
            "command": "diag", "expect": r"WCET us",
            "timeout": 5, "notes": "REQ:TIMING"
        },
        {
            "id": "T27", "name": "Stress mode OFF",
            "command": "stress off", "expect": r"Stress OFF",
            "timeout": 3, "notes": "REQ:STRESS"
        },

        # ── Race Condition Demo ───────────────────────────
        {
            "id": "T28", "name": "Race unsafe demo runs",
            "command": "race unsafe", "expect": r"RACE.*unsafe|RACE.*result",
            "timeout": 8, "notes": "REQ:RACE_CONDITION"
        },
        {
            "id": "T29", "name": "Race mutex demo — result is 0",
            "command": "race mutex", "expect": r"expected 0.*0|result.*: 0",
            "timeout": 8, "notes": "REQ:RACE_CONDITION"
        },

        # ── Priority Inversion Demo ───────────────────────
        {
            "id": "T30", "name": "Priority inversion demo ON",
            "command": "prio inversion on", "expect": r"PrioInv ON",
            "timeout": 3, "notes": "REQ:PRIO_INVERSION"
        },
        {
            "id": "T31", "name": "Priority inversion — high-prio gets mutex",
            "command": "", "expect": r"High-prio task got mutex|PRIOINV.*High-prio",
            "timeout": 8, "notes": "REQ:PRIO_INVERSION"
        },
        {
            "id": "T32", "name": "Priority inversion demo OFF",
            "command": "prio inversion off", "expect": r"PrioInv OFF",
            "timeout": 3, "notes": "REQ:PRIO_INVERSION"
        },

        # ── Timing / WCET Log Parser ──────────────────────
        {
            "id": "T33", "name": "Timing: WCET values present in JSON log",
            "command": "", "expect": r"wcet_analog_us|wcet_comm_us",
            "timeout": 6, "notes": "REQ:TIMING"
        },
        {
            "id": "T34", "name": "Timing: jitter values present in JSON log",
            "command": "", "expect": r"jitter_analog_us",
            "timeout": 6, "notes": "REQ:TIMING"
        },
        {
            "id": "T35", "name": "Timing: semaphore wakeup latency reported",
            "command": "diag", "expect": r"Sem wakeup latency",
            "timeout": 4, "notes": "REQ:SEMAPHORE"
        },

        # ── CPU Load ─────────────────────────────────────
        {
            "id": "T36", "name": "CPU load fault ON",
            "command": "fault cpuload on", "expect": r"CPULoad ON",
            "timeout": 3, "notes": "REQ:CPU_LOAD"
        },
        {
            "id": "T37", "name": "CPU load fault OFF",
            "command": "fault cpuload off", "expect": r"CPULoad OFF",
            "timeout": 3, "notes": "REQ:CPU_LOAD"
        },

        # ── Delayed Processing ────────────────────────────
        {
            "id": "T38", "name": "Delayed processing ON",
            "command": "fault delayproc on", "expect": r"DelayProc ON",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T39", "name": "Delayed processing — blocking log appears",
            "command": "", "expect": r"DelayedProcessing.*blocking",
            "timeout": 8, "notes": "REQ:FAULT_INJECTION"
        },
        {
            "id": "T40", "name": "Delayed processing OFF",
            "command": "fault delayproc off", "expect": r"DelayProc OFF",
            "timeout": 3, "notes": "REQ:FAULT_INJECTION"
        },
    ]

# ─────────────────────────────────────────────
# Log Parsers (Bonus)
# ─────────────────────────────────────────────

def parse_timing_from_log(log_lines: list[str]) -> list[dict]:
    """Extract WCET / jitter / missed-deadline values from [LOG] JSON lines."""
    records = []
    for line in log_lines:
        m = re.search(r'\[LOG\]\s+(\{.*\})', line)
        if not m:
            continue
        try:
            data = json.loads(m.group(1))
        except json.JSONDecodeError:
            continue
        timing = {k: data[k] for k in data if k.startswith(("wcet_", "jitter_", "missed_dl_", "sem_wakeup"))}
        if timing:
            timing["_timestamp"] = datetime.now().isoformat()
            records.append(timing)
    return records

def parse_queue_metrics(log_lines: list[str]) -> dict:
    """Extract latest queue occupancy / dropped / overwrite counts."""
    metrics = {"queue_occupancy": 0, "dropped_messages": 0, "queue_overwrites": 0}
    for line in log_lines:
        m = re.search(r'\[LOG\]\s+(\{.*\})', line)
        if not m:
            continue
        try:
            data = json.loads(m.group(1))
        except json.JSONDecodeError:
            continue
        for key in metrics:
            if key in data:
                metrics[key] = data[key]
    return metrics

def parse_requirement_coverage(results: list[TestResult]) -> list[dict]:
    """Build a requirement → pass/fail coverage table."""
    coverage: dict[str, dict] = {}
    for r in results:
        req = r.notes or "UNKNOWN"
        if req not in coverage:
            coverage[req] = {"requirement": req, "total": 0, "passed": 0, "failed": 0, "test_ids": []}
        coverage[req]["total"]  += 1
        coverage[req]["passed"] += 1 if r.passed else 0
        coverage[req]["failed"] += 0 if r.passed else 1
        coverage[req]["test_ids"].append(r.test_id)
    return list(coverage.values())

# ─────────────────────────────────────────────
# Report Generators
# ─────────────────────────────────────────────

def save_csv_report(session: TestSession, path: str):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "test_id", "name", "command", "expected",
            "passed", "actual", "duration_s", "timestamp", "notes"
        ])
        writer.writeheader()
        for r in session.results:
            writer.writerow(asdict(r))
    print(f"[REPORT] CSV saved → {path}")

def save_json_report(session: TestSession, timing: list[dict],
                     queue: dict, coverage: list[dict], path: str):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    payload = {
        "session":  asdict(session),
        "timing_samples": timing,
        "queue_metrics":  queue,
        "requirement_coverage": coverage,
    }
    with open(path, "w") as f:
        json.dump(payload, f, indent=2)
    print(f"[REPORT] JSON saved → {path}")

def save_timing_csv(timing: list[dict], path: str):
    if not timing:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    keys = sorted({k for row in timing for k in row})
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(timing)
    print(f"[REPORT] Timing CSV saved → {path}")

def save_coverage_csv(coverage: list[dict], path: str):
    if not coverage:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["requirement","total","passed","failed","test_ids"])
        writer.writeheader()
        for row in coverage:
            row2 = dict(row)
            row2["test_ids"] = " ".join(row2["test_ids"])
            writer.writerow(row2)
    print(f"[REPORT] Coverage CSV saved → {path}")

def print_summary(session: TestSession):
    print("\n" + "═"*55)
    print(f"  TEST SUMMARY  —  {session.session_id}")
    print("═"*55)
    print(f"  Port:    {session.port}  @  {session.baud}")
    print(f"  Total:   {session.total}")
    print(f"  Passed:  {session.passed}  ✓")
    print(f"  Failed:  {session.failed}  ✗")
    pct = (session.passed / session.total * 100) if session.total else 0
    print(f"  Score:   {pct:.1f}%")
    print("─"*55)
    for r in session.results:
        icon = "✓" if r.passed else "✗"
        print(f"  [{icon}] {r.test_id}  {r.name}")
    print("═"*55 + "\n")

# ─────────────────────────────────────────────
# Main Runner
# ─────────────────────────────────────────────

def run(port: str, baud: int, skip_flash: bool):
    session_id = datetime.now().strftime("session_%Y%m%d_%H%M%S")
    ts_start   = datetime.now().isoformat()

    # Step 1 — Optional flash
    if not skip_flash:
        print("[FLASH] Running: pio run --target upload")
        ret = os.system(f"pio run --target upload --upload-port {port}")
        if ret != 0:
            print("[ERROR] Flash failed — aborting")
            sys.exit(1)
        time.sleep(3)

    # Step 2 — Connect serial
    monitor = SerialMonitor(port, baud)
    if not monitor.connect():
        sys.exit(1)

    # Step 3 + 4 — Send commands & collect logs
    suite   = build_test_suite()
    results: list[TestResult] = []

    # Drain boot messages first
    print("[INFO] Draining boot messages (5s)…")
    monitor.drain(5.0)

    for test in suite:
        cmd     = test["command"]
        pattern = test["expect"]
        timeout = test.get("timeout", DEFAULT_TIMEOUT)

        if cmd:
            time.sleep(0.3)
            monitor.send(cmd)

        t0 = time.time()
        found, actual = monitor.read_until(pattern, timeout)
        elapsed = round(time.time() - t0, 3)

        result = TestResult(
            test_id    = test["id"],
            name       = test["name"],
            command    = cmd,
            expected   = pattern,
            passed     = found,
            actual     = actual[-300:] if len(actual) > 300 else actual,
            duration_s = elapsed,
            timestamp  = datetime.now().isoformat(),
            notes      = test.get("notes", ""),
        )
        results.append(result)
        status = "PASS" if found else "FAIL"
        print(f"  [{status}] {test['id']} — {test['name']}")

    monitor.disconnect()

    # Step 5 — Evaluate
    session = TestSession(
        session_id  = session_id,
        port        = port,
        baud        = baud,
        started_at  = ts_start,
        finished_at = datetime.now().isoformat(),
        results     = results,
        total       = len(results),
        passed      = sum(1 for r in results if r.passed),
        failed      = sum(1 for r in results if not r.passed),
    )

    print_summary(session)

    # Bonus parsers
    timing_records = parse_timing_from_log(monitor.log_lines)
    queue_metrics  = parse_queue_metrics(monitor.log_lines)
    coverage       = parse_requirement_coverage(results)

    # Step 6 — Generate reports
    os.makedirs(LOG_DIR, exist_ok=True)
    prefix = f"{LOG_DIR}/{session_id}"
    save_csv_report(session,  f"{prefix}_results.csv")
    save_timing_csv(timing_records, f"{prefix}_timing.csv")
    save_coverage_csv(coverage,     f"{prefix}_coverage.csv")
    save_json_report(session, timing_records, queue_metrics, coverage,
                     f"{prefix}_full_report.json")

    # Exit code for CI
    sys.exit(0 if session.failed == 0 else 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ESP32 RTOS Automated Test Runner")
    parser.add_argument("--port",        default=DEFAULT_PORT, help="Serial port (e.g. COM3 or /dev/ttyUSB0)")
    parser.add_argument("--baud",        default=DEFAULT_BAUD, type=int)
    parser.add_argument("--skip-flash",  action="store_true", help="Skip PlatformIO flash step")
    args = parser.parse_args()

    run(args.port, args.baud, args.skip_flash)
