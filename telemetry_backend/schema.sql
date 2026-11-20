-- TimescaleDB / PostgreSQL schema for Edge AI Telemetry Node
-- Aligns with telemetry_backend/mqtt_to_timescaledb.py

CREATE EXTENSION IF NOT EXISTS timescaledb;

CREATE TABLE IF NOT EXISTS telemetry_events (
    time        TIMESTAMPTZ NOT NULL,
    state_code  SMALLINT    NOT NULL,
    state_label VARCHAR(32) NOT NULL
);

SELECT create_hypertable('telemetry_events', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS idx_telemetry_events_state_time
    ON telemetry_events (state_code, time DESC);

-- Example Grafana time-series query:
-- SELECT time AS "time", state_code, state_label
-- FROM telemetry_events
-- WHERE $__timeFilter(time)
-- ORDER BY time ASC;
