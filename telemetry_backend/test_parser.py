"""Unit tests for Edge AI Telemetry Node MQTT parser and SQL bridge logic."""

import os
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(__file__))

import mqtt_receiver  # noqa: E402
import mqtt_to_timescaledb  # noqa: E402


def make_frame(seq_id: int, state_code: int) -> bytes:
    """Build a valid 4-byte [0xAA][SEQ][STATE][CRC8] frame."""
    header = bytes([0xAA, seq_id & 0xFF, state_code & 0xFF])
    return header + bytes([mqtt_to_timescaledb.compute_crc8(header)])


class FakeCursor:
    def __init__(self):
        self.rows = []

    def execute(self, query, params):
        self.rows.append((query, params))


class FakeConn:
    def __init__(self):
        self.commits = 0
        self.rollbacks = 0

    def commit(self):
        self.commits += 1

    def rollback(self):
        self.rollbacks += 1


class TestTelemetryParser(unittest.TestCase):
    def test_crc8_datasheet_vectors(self):
        self.assertEqual(mqtt_to_timescaledb.compute_crc8(bytes([0x80, 0x00])), 0xA2)
        self.assertEqual(mqtt_to_timescaledb.compute_crc8(bytes([0x66, 0x66])), 0x93)
        self.assertEqual(mqtt_to_timescaledb.compute_crc8(bytes([0x00, 0x00])), 0x81)
        self.assertEqual(mqtt_to_timescaledb.compute_crc8(bytes([0xFF, 0xFF])), 0xAC)
        self.assertEqual(
            mqtt_receiver.compute_crc8(bytes([0x80, 0x00])),
            mqtt_to_timescaledb.compute_crc8(bytes([0x80, 0x00])),
        )

    def test_valid_4_byte_frames(self):
        expected = [
            (0, 0x00, "NORMAL"),
            (42, 0x01, "ANOMALY"),
            (255, 0x02, "CALIBRATING"),
            (7, 0xFF, "SENSOR_FAULT"),
        ]
        for seq_id, code, label in expected:
            frame = make_frame(seq_id, code)
            self.assertEqual(mqtt_to_timescaledb.parse_state_payload(frame), (seq_id, code, label))

    def test_rejects_corrupt_or_invalid_frames(self):
        valid = make_frame(10, 0x01)
        bad_crc = valid[:3] + bytes([valid[3] ^ 0x01])
        bad_sync = bytes([0x55, 10, 0x01]) + bytes([mqtt_to_timescaledb.compute_crc8(bytes([0x55, 10, 0x01]))])
        unknown_state = make_frame(10, 0x55)

        self.assertIsNone(mqtt_to_timescaledb.parse_state_payload(bad_crc))
        self.assertIsNone(mqtt_to_timescaledb.parse_state_payload(bad_sync))
        self.assertIsNone(mqtt_to_timescaledb.parse_state_payload(unknown_state))
        self.assertIsNone(mqtt_to_timescaledb.parse_state_payload(valid[:3]))
        self.assertIsNone(mqtt_to_timescaledb.parse_state_payload(b""))
        self.assertIsNone(mqtt_to_timescaledb.parse_state_payload(b"invalid_text"))

    def test_bench_fallback_formats(self):
        self.assertEqual(mqtt_to_timescaledb.parse_state_payload(b"\x00"), (None, 0, "NORMAL"))
        self.assertEqual(mqtt_to_timescaledb.parse_state_payload(b"1"), (None, 1, "ANOMALY"))
        self.assertEqual(mqtt_to_timescaledb.parse_state_payload(b"ANOMALY:1"), (None, 1, "ANOMALY"))

    def test_on_message_commits_valid_frame_and_skips_bad(self):
        conn = FakeConn()
        cursor = FakeCursor()
        on_message = mqtt_to_timescaledb.make_on_message(conn, cursor)

        on_message(None, None, SimpleNamespace(topic="edge_ai/telemetry", payload=make_frame(255, 0x00)))
        on_message(None, None, SimpleNamespace(topic="edge_ai/telemetry", payload=make_frame(0, 0x01)))
        on_message(None, None, SimpleNamespace(topic="edge_ai/telemetry", payload=b"\xAA\x01\x01\x00"))

        self.assertEqual(conn.commits, 2)
        self.assertEqual(len(cursor.rows), 2)
        self.assertEqual(cursor.rows[0][1][1:], (255, 0x00, "NORMAL"))
        self.assertEqual(cursor.rows[1][1][1:], (0, 0x01, "ANOMALY"))


if __name__ == "__main__":
    unittest.main()
