"""Lightweight MQTT subscriber for monitoring Edge AI telemetry payloads."""

import os
import sys
import paho.mqtt.client as mqtt

BROKER_HOST = os.getenv("MQTT_BROKER", "localhost")
BROKER_PORT = int(os.getenv("MQTT_PORT", "1883"))
TOPIC = os.getenv("MQTT_TOPIC", "edge_ai/telemetry")


def on_connect(client, userdata, flags, reason_code, properties=None):
    """Subscribe to the telemetry topic once the broker acknowledges connection."""
    print(f"[*] Connected to MQTT broker ({BROKER_HOST}:{BROKER_PORT}), code={reason_code}")
    client.subscribe(TOPIC)
    print(f"[*] Subscribed to topic: {TOPIC}")


def on_message(client, userdata, msg):
    """Print incoming UTF-8 or raw 1-byte state flag payloads."""
    raw = msg.payload
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
