"""Lightweight MQTT subscriber for monitoring Edge AI telemetry frames."""

import os
import sys
import paho.mqtt.client as mqtt

BROKER_HOST = os.getenv("MQTT_BROKER", "localhost")
BROKER_PORT = int(os.getenv("MQTT_PORT", "1883"))
TOPIC = os.getenv("MQTT_TOPIC", "edge_ai/telemetry")
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


def on_connect(client, userdata, flags, reason_code, properties=None):
    """Subscribe to the telemetry topic once the broker acknowledges connection."""
    print(f"[*] Connected to MQTT broker ({BROKER_HOST}:{BROKER_PORT}), code={reason_code}")
    client.subscribe(TOPIC)
    print(f"[*] Subscribed to topic: {TOPIC}")


def on_message(client, userdata, msg):
    """Decode 4-byte CRC-8 frames or print UTF-8/raw test payloads."""
    raw = msg.payload
    if len(raw) == 4 and raw[0] == FRAME_SYNC_BYTE:
        crc_ok = compute_crc8(raw[:3]) == raw[3]
        seq_id, code = raw[1], raw[2]
        label = STATE_LABELS.get(code, f"UNKNOWN_0x{code:02X}")
        status = "CRC_OK" if crc_ok else "CRC_ERR"
        print(f"MQTT [{msg.topic}]: seq={seq_id} state={label} (0x{code:02X}) [{status}]")
        return

    try:
        decoded = raw.decode("utf-8").strip()
        print(f"MQTT [{msg.topic}]: {decoded}")
    except UnicodeDecodeError:
        print(f"MQTT [{msg.topic}]: 0x{raw.hex()}")


def main():
    """Start the blocking MQTT listener loop until interrupted."""
    print("[*] Initializing MQTT telemetry monitor...")
    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    try:
        client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[*] Disconnecting from broker...")
        client.disconnect()
    except OSError as exc:
        print(f"[!] Could not reach broker at {BROKER_HOST}:{BROKER_PORT}: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
