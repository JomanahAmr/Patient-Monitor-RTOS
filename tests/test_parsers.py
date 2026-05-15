"""
Unit tests for serial_test_runner parser functions.
These run entirely offline — no ESP32 hardware required.
Used in GitHub Actions CI (unit-tests job).
"""

import pytest
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from serial_test_runner import (
    parse_timing_from_log,
    parse_queue_metrics,
    parse_requirement_coverage,
    TestResult,
)

# ─── Fixtures — synthetic log lines ───────────────────

SAMPLE_LOG_JSON = (
    '[LOG] {"heart_rate":72,"oxygen_saturation":98,"temperature_celsius":36.5,'
    '"temperature_fahrenheit":97.7,"respiratory_rate":16,"emergency_touch":false,'
    '"risk_label":"LOW_RISK","risk_probability":0.1,"wifi":"connected",'
    '"queue_occupancy":1,"dropped_messages":0,"queue_overwrites":0,'
    '"wcet_analog_us":450,"wcet_comm_us":800,"wcet_proc_us":3200,"wcet_log_us":600,'
    '"jitter_analog_us":120,"jitter_comm_us":230,"jitter_proc_us":90,"jitter_log_us":55,'
    '"missed_dl_analog":0,"missed_dl_comm":0,"missed_dl_proc":0,"missed_dl_log":0,'
    '"sem_wakeup_latency_us":85,"touch_events_accepted":2,"touch_events_rejected":0,'
    '"fault_analog":false,"fault_i2c":false,"fault_api":false,"stress_mode":false}'
)

OVERFLOW_LOG = (
    '[LOG] {"heart_rate":65,"oxygen_saturation":97,"temperature_celsius":36.2,'
    '"respiratory_rate":14,"emergency_touch":false,"risk_label":"LOW_RISK",'
    '"risk_probability":0.1,"wifi":"connected",'
    '"queue_occupancy":5,"dropped_messages":3,"queue_overwrites":2,'
    '"wcet_analog_us":500,"wcet_comm_us":900,"wcet_proc_us":3500,"wcet_log_us":700,'
    '"jitter_analog_us":200,"jitter_comm_us":300,"jitter_proc_us":150,"jitter_log_us":80,'
    '"missed_dl_analog":1,"missed_dl_comm":0,"missed_dl_proc":1,"missed_dl_log":0,'
    '"sem_wakeup_latency_us":90,"touch_events_accepted":5,"touch_events_rejected":1,'
    '"fault_analog":false,"fault_i2c":false,"fault_api":false,"stress_mode":true}'
)

SAMPLE_LINES = [
    "ESP32 RTOS Patient Monitor Started",
    "All RTOS tasks created",
    SAMPLE_LOG_JSON,
    "[QUEUE] Occupancy: 1",
    "[I2C] HR=75 SpO2=98",
    OVERFLOW_LOG,
]

# ─── parse_timing_from_log ─────────────────────────────

class TestParseTimingFromLog:
    def test_extracts_two_records(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        assert len(records) == 2

    def test_wcet_analog_present(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        assert "wcet_analog_us" in records[0]

    def test_wcet_analog_value(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        assert records[0]["wcet_analog_us"] == 450

    def test_jitter_comm_present(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        assert "jitter_comm_us" in records[0]

    def test_jitter_comm_value(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        assert records[0]["jitter_comm_us"] == 230

    def test_sem_wakeup_latency_present(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        assert "sem_wakeup_latency_us" in records[0]

    def test_missed_deadlines_analog(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        # second record (overflow log) has missed_dl_analog=1
        assert records[1]["missed_dl_analog"] == 1

    def test_wcet_proc_second_record(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        assert records[1]["wcet_proc_us"] == 3500

    def test_empty_lines_returns_empty(self):
        assert parse_timing_from_log([]) == []

    def test_non_log_lines_ignored(self):
        lines = ["random text", "[QUEUE] 1", "[I2C] HR=70"]
        assert parse_timing_from_log(lines) == []

    def test_timestamp_key_added(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        assert "_timestamp" in records[0]

# ─── parse_queue_metrics ──────────────────────────────

class TestParseQueueMetrics:
    def test_returns_latest_occupancy(self):
        """Should return values from the LAST matching log line."""
        metrics = parse_queue_metrics(SAMPLE_LINES)
        assert metrics["queue_occupancy"] == 5  # from overflow log (last)

    def test_returns_dropped_messages(self):
        metrics = parse_queue_metrics(SAMPLE_LINES)
        assert metrics["dropped_messages"] == 3

    def test_returns_queue_overwrites(self):
        metrics = parse_queue_metrics(SAMPLE_LINES)
        assert metrics["queue_overwrites"] == 2

    def test_defaults_when_no_log_lines(self):
        metrics = parse_queue_metrics([])
        assert metrics == {"queue_occupancy": 0, "dropped_messages": 0, "queue_overwrites": 0}

    def test_single_log_line(self):
        metrics = parse_queue_metrics([SAMPLE_LOG_JSON])
        assert metrics["queue_occupancy"] == 1
        assert metrics["dropped_messages"] == 0

# ─── parse_requirement_coverage ───────────────────────

def make_results(*args):
    """Helper: list of TestResult(passed=..., notes=...)"""
    results = []
    for i, (passed, notes) in enumerate(args):
        results.append(TestResult(
            test_id=f"T{i+1:02d}", name=f"Test {i+1}",
            command="", expected="",
            passed=passed, notes=notes
        ))
    return results

class TestRequirementCoverage:
    def test_single_req_all_pass(self):
        results = make_results((True, "REQ:BOOT"), (True, "REQ:BOOT"))
        cov = parse_requirement_coverage(results)
        assert len(cov) == 1
        assert cov[0]["requirement"] == "REQ:BOOT"
        assert cov[0]["passed"] == 2
        assert cov[0]["failed"] == 0

    def test_mixed_pass_fail(self):
        results = make_results(
            (True,  "REQ:FAULT_INJECTION"),
            (False, "REQ:FAULT_INJECTION"),
            (True,  "REQ:FAULT_INJECTION"),
        )
        cov = parse_requirement_coverage(results)
        assert cov[0]["passed"] == 2
        assert cov[0]["failed"] == 1

    def test_multiple_requirements(self):
        results = make_results(
            (True,  "REQ:BOOT"),
            (True,  "REQ:QUEUE_OVERFLOW"),
            (False, "REQ:QUEUE_OVERFLOW"),
            (True,  "REQ:TIMING"),
        )
        cov = {r["requirement"]: r for r in parse_requirement_coverage(results)}
        assert cov["REQ:BOOT"]["total"] == 1
        assert cov["REQ:QUEUE_OVERFLOW"]["total"] == 2
        assert cov["REQ:TIMING"]["passed"] == 1

    def test_test_ids_recorded(self):
        results = make_results((True, "REQ:MUTEX"), (False, "REQ:MUTEX"))
        cov = parse_requirement_coverage(results)
        assert "T01" in cov[0]["test_ids"]
        assert "T02" in cov[0]["test_ids"]

    def test_empty_results(self):
        assert parse_requirement_coverage([]) == []

    def test_unknown_notes_grouped(self):
        results = make_results((True, ""), (False, ""))
        cov = parse_requirement_coverage(results)
        assert cov[0]["requirement"] == "UNKNOWN"

# ─── Bonus: timing thresholds ─────────────────────────

class TestTimingThresholds:
    """Validate that parsed WCET values are within acceptable ranges."""

    WCET_LIMITS = {
        "wcet_analog_us":  50_000,   # 50 ms max
        "wcet_comm_us":   100_000,   # 100 ms max (I2C)
        "wcet_proc_us":  5_000_000,  # 5 s max (includes API call)
        "wcet_log_us":    200_000,   # 200 ms max
    }

    def test_wcet_analog_within_limit(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        for r in records:
            assert r.get("wcet_analog_us", 0) <= self.WCET_LIMITS["wcet_analog_us"], \
                f"wcet_analog_us={r['wcet_analog_us']} exceeds limit"

    def test_wcet_comm_within_limit(self):
        records = parse_timing_from_log(SAMPLE_LINES)
        for r in records:
            assert r.get("wcet_comm_us", 0) <= self.WCET_LIMITS["wcet_comm_us"]

    def test_no_missed_deadlines_in_normal_log(self):
        records = parse_timing_from_log([SAMPLE_LOG_JSON])
        assert records[0]["missed_dl_analog"] == 0
        assert records[0]["missed_dl_comm"] == 0

    def test_missed_deadlines_detected_under_stress(self):
        records = parse_timing_from_log([OVERFLOW_LOG])
        assert records[0]["missed_dl_analog"] == 1
        assert records[0]["missed_dl_proc"] == 1
