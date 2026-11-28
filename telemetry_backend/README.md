# Telemetry Backend (MQTT → TimescaleDB → Grafana)

Host-side ingestion pipeline for the **Edge AI Telemetry Node**. It subscribes to deterministic 4-byte CRC-8 telemetry frames published by the ESP32 gateway over Mosquitto MQTT, verifies packet integrity and sequence continuity, and stores time-series events in PostgreSQL / TimescaleDB for Grafana visualization.

## Components

* `mqtt_receiver.py` — Lightweight console monitor subscribing to `MQTT_TOPIC` (decodes 4-byte CRC-8 frames as well as UTF-8/raw test inputs).
* `mqtt_to_timescaledb.py` — CRC-8 verifying, sequence-tracking SQL ingestion bridge writing parsed events into `telemetry_events`.
* `schema.sql` — TimescaleDB extension, `telemetry_events` table (`time`, `seq_id`, `state_code`, `state_label`), hypertable conversion, and query index.
* `.env.example` — Template for MQTT broker and database environment variables.
* `requirements.txt` — Python dependencies (`paho-mqtt`, `psycopg2-binary`).

## Deterministic 4-Byte Frame Contract

In line with the STM32-to-ESP32 air-gap boundary, raw environmental samples stay in STM32 SRAM and only fixed 4-byte frames (`[SYNC 0xAA][SEQ_ID][STATE_FLAG][CRC8]`) cross UART4 and MQTT (`edge_ai/telemetry`):

| Byte(s) | Field | Details |
| :--- | :--- | :--- |
| `0` | `SYNC` | Fixed `0xAA` framing header |
| `1` | `SEQ_ID` | Rolling `uint8_t` (`0..255`) for dropped-frame detection |
| `2` | `STATE_FLAG` | `0x00` `NORMAL`, `0x01` `ANOMALY`, `0x02` `CALIBRATING`, `0xFF` `SENSOR_FAULT` |
| `3` | `CRC8` | CRC-8 (`poly 0x31`, `init 0xFF`) over bytes `0..2` |

For bench testing without binary framing, `mqtt_to_timescaledb.py` also accepts ASCII integers (e.g., `1`) or `LABEL:CODE` strings (e.g., `ANOMALY:1`) with `seq_id = NULL`.

## Quick Start

1. **Install Python dependencies:**
   ```bash
   pip install -r requirements.txt
   ```

2. **Configure environment variables** (copy `.env.example` to `.env` or export in your shell):
   ```bash
   export MQTT_BROKER=localhost
   export MQTT_PORT=1883
   export MQTT_TOPIC=edge_ai/telemetry
   export PGHOST=localhost
   export PGPORT=5432
   export PGDATABASE=postgres
   export PGUSER=postgres
   export PGPASSWORD=<your-db-password>
   ```

3. **Initialize TimescaleDB schema:**
   ```bash
   psql -h "$PGHOST" -U "$PGUSER" -d "$PGDATABASE" -f schema.sql
   ```

4. **Run the monitor or ingestion bridge:**
   ```bash
   python mqtt_receiver.py
   # or
   python mqtt_to_timescaledb.py
   ```
