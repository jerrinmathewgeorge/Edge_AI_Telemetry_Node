"""MQTT-to-TimescaleDB ingestion bridge for Edge AI telemetry state flags."""

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

STATE_LABELS = {
    0x00: "NORMAL",
    0x01: "ANOMALY",
    0x02: "CALIBRATING",
    0xFF: "SENSOR_FAULT",
}


def parse_state_payload(payload: bytes) -> tuple[int, str] | None:
    """Parse a 1-byte binary state flag or formatted text flag into (code, label)."""
    if not payload:
        return None

    if len(payload) == 1 and (payload[0] < 0x20 or payload[0] == 0xFF):
        code = payload[0]
        return code, STATE_LABELS.get(code, f"STATE_{code}")

    try:
        text = payload.decode("utf-8").strip()
    except UnicodeDecodeError:
        if len(payload) == 1:
            code = payload[0]
            return code, STATE_LABELS.get(code, f"STATE_{code}")
        return None

    if text.isdigit():
        code = int(text)
        return code, STATE_LABELS.get(code, f"STATE_{code}")

    if ":" in text:
        raw_label, raw_val = text.split(":", 1)
        try:
            code = int(raw_val.strip())
        except ValueError:
            return None
        label = raw_label.strip().upper() or STATE_LABELS.get(code, f"STATE_{code}")
        return code, label[:32]

    if len(payload) == 1:
        code = payload[0]
        return code, STATE_LABELS.get(code, f"STATE_{code}")

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
            time TIMESTAMPTZ NOT NULL,
            state_code SMALLINT NOT NULL,
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
    """Create the MQTT v2 message callback bound to an active DB connection."""
    def on_message(client, userdata, msg):
        parsed = parse_state_payload(msg.payload)
        if parsed is None:
            print(f"[!] Ignored malformed payload on {msg.topic}: {msg.payload!r}")
            return

        state_code, state_label = parsed
        timestamp = datetime.now(timezone.utc)
        try:
            db_cursor.execute(
                "INSERT INTO telemetry_events (time, state_code, state_label) VALUES (%s, %s, %s)",
                (timestamp, state_code, state_label),
            )
            db_conn.commit()
            print(f"DB COMMIT [{timestamp.isoformat()}] -> {state_label} (0x{state_code:02X})")
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
