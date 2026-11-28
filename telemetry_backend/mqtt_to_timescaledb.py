"""MQTT-to-TimescaleDB ingestion bridge for 4-byte CRC-8 Edge AI telemetry frames."""

import os
import sys
from datetime import datetime, timezone
import paho.mqtt.client as mqtt
import psycopg2

BROKER_HOST = os.getenv("MQTT_BROKER", "localhost")
BROKER_PORT = int(os.getenv("MQTT_PORT", "1883"))
TOPIC = os.getenv("MQTT_TOPIC", "edge_ai/telemetry")

DB_HOST = os.getenv("PGHOST", "localhost")
DB_PORT = int(os.getenv("PGPORT", "5432"))
DB_NAME = os.getenv("PGDATABASE", "postgres")
DB_USER = os.getenv("PGUSER", "postgres")
DB_PASS = os.getenv("PGPASSWORD")

FRAME_SYNC_BYTE = 0xAA

STATE_LABELS = {
    0x00: "NORMAL",
    0x01: "ANOMALY",
    0x02: "CALIBRATING",
    0xFF: "SENSOR_FAULT",
}


def compute_crc8(data: bytes) -> int:
    """Compute Sensirion-compatible CRC-8 (poly 0x31, init 0xFF)."""
    crc = 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def parse_state_payload(payload: bytes) -> tuple[int | None, int, str] | None:
    """Parse a 4-byte CRC-8 frame [0xAA][SEQ][STATE][CRC8] or fallback test flag."""
    if not payload:
        return None

    if len(payload) == 4 and payload[0] == FRAME_SYNC_BYTE:
        if compute_crc8(payload[:3]) != payload[3]:
            return None
        seq_id = payload[1]
        code = payload[2]
        label = STATE_LABELS.get(code)
        if label is None:
            return None
        return seq_id, code, label

    if len(payload) == 1 and (payload[0] < 0x20 or payload[0] == 0xFF):
        code = payload[0]
        return None, code, STATE_LABELS.get(code, f"STATE_{code}")

    try:
        text = payload.decode("utf-8").strip()
    except UnicodeDecodeError:
        if len(payload) == 1:
            code = payload[0]
            return None, code, STATE_LABELS.get(code, f"STATE_{code}")
        return None

    if text.isdigit():
        code = int(text)
        return None, code, STATE_LABELS.get(code, f"STATE_{code}")

    if ":" in text:
        raw_label, raw_val = text.split(":", 1)
        try:
            code = int(raw_val.strip())
        except ValueError:
            return None
        label = raw_label.strip().upper() or STATE_LABELS.get(code, f"STATE_{code}")
        return None, code, label[:32]

    if len(payload) == 1:
        code = payload[0]
        return None, code, STATE_LABELS.get(code, f"STATE_{code}")

    return None


def init_db():
    """Connect to PostgreSQL/TimescaleDB and ensure the telemetry table exists."""
    if not DB_PASS:
        raise RuntimeError("PGPASSWORD environment variable is not set (see .env.example).")

    conn = psycopg2.connect(
        host=DB_HOST,
        port=DB_PORT,
        database=DB_NAME,
        user=DB_USER,
        password=DB_PASS,
    )
    cursor = conn.cursor()
    cursor.execute(
        """
        CREATE TABLE IF NOT EXISTS telemetry_events (
            time        TIMESTAMPTZ NOT NULL,
            seq_id      SMALLINT,
            state_code  SMALLINT    NOT NULL,
            state_label VARCHAR(32) NOT NULL
        );
        """
    )
    conn.commit()
    return conn, cursor


def make_on_connect():
    """Create the MQTT v2 connection callback."""
    def on_connect(client, userdata, flags, reason_code, properties=None):
        print(f"[*] Connected to MQTT broker ({BROKER_HOST}:{BROKER_PORT}), code={reason_code}")
        client.subscribe(TOPIC)
        print(f"[*] Subscribed to topic: {TOPIC}")

    return on_connect


def make_on_message(db_conn, db_cursor):
    """Create the MQTT v2 message callback with sequence-gap tracking."""
    tracker = {"last_seq": None}

    def on_message(client, userdata, msg):
        parsed = parse_state_payload(msg.payload)
        if parsed is None:
            print(f"[!] Rejected invalid/corrupt payload on {msg.topic}: {msg.payload!r}")
            return

        seq_id, state_code, state_label = parsed
        if seq_id is not None:
            if tracker["last_seq"] is not None:
                expected = (tracker["last_seq"] + 1) & 0xFF
                if seq_id != expected:
                    print(f"[!] Sequence gap detected: expected seq={expected}, got seq={seq_id}")
            tracker["last_seq"] = seq_id

        timestamp = datetime.now(timezone.utc)
        try:
            db_cursor.execute(
                "INSERT INTO telemetry_events (time, seq_id, state_code, state_label) "
                "VALUES (%s, %s, %s, %s)",
                (timestamp, seq_id, state_code, state_label),
            )
            db_conn.commit()
            seq_str = f"seq={seq_id} " if seq_id is not None else ""
            print(f"DB COMMIT [{timestamp.isoformat()}] -> {seq_str}{state_label} (0x{state_code:02X})")
        except Exception as exc:
            db_conn.rollback()
            print(f"[!] Database insert failed: {exc}", file=sys.stderr)

    return on_message


def main():
    """Initialize DB and MQTT client, then ingest telemetry until interrupted."""
    print("[*] Initializing PostgreSQL/TimescaleDB connection...")
    try:
        db_conn, db_cursor = init_db()
        print("[*] Database connection ready.")
    except Exception as exc:
        print(f"[!] Database initialization failed: {exc}", file=sys.stderr)
        sys.exit(1)

    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = make_on_connect()
    client.on_message = make_on_message(db_conn, db_cursor)

    try:
        client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[*] Shutting down telemetry bridge...")
    except OSError as exc:
        print(f"[!] Could not reach broker at {BROKER_HOST}:{BROKER_PORT}: {exc}", file=sys.stderr)
        sys.exit(1)
    finally:
        client.disconnect()
        db_cursor.close()
        db_conn.close()


if __name__ == "__main__":
    main()
