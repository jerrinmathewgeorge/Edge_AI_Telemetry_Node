# Telemetry Backend (MQTT → TimescaleDB → Grafana)

Host-side ingestion pipeline for the **Edge AI Telemetry Node**. It subscribes to abstracted anomaly state flags published by the ESP32 gateway over Mosquitto MQTT and stores time-series events in PostgreSQL / TimescaleDB for Grafana visualization.

## Components

* `mqtt_receiver.py` — Lightweight console monitor subscribing to `MQTT_TOPIC` (supports both raw 1-byte hex flags and UTF-8 test strings).
* `mqtt_to_timescaledb.py` — Parameterized SQL ingestion bridge writing parsed events into `telemetry_events`.
* `schema.sql` — TimescaleDB extension, `telemetry_events` table, hypertable conversion, and query index.
* `.env.example` — Template for MQTT broker and database environment variables.
* `requirements.txt` — Python dependencies (`paho-mqtt`, `psycopg2-binary`).

## 1-Byte State Flag Contract

In line with the STM32-to-ESP32 air-gap boundary, raw environmental samples stay in STM32 SRAM and only abstracted state codes are published to `edge_ai/telemetry`:

| Byte Code | Label | Meaning |
| :--- | :--- | :--- |
| `0x00` | `NORMAL` | Baseline air quality within EWMA + TinyML bounds |
| `0x01` | `ANOMALY` | Residual spike flagged by TinyML inference |
| `0x02` | `CALIBRATING` | Initial EWMA / MOX baseline warm-up window |
| `0xFF` | `SENSOR_FAULT` | Physical I2C read/CRC fault reported by edge node |

For bench testing without binary framing, `mqtt_to_timescaledb.py` also accepts ASCII integers (e.g., `1`) or `LABEL:CODE` strings (e.g., `ANOMALY:1`).

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
