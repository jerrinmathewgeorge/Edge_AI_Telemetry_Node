# ESP32 Gateway (Air-Gapped UART-to-MQTT Bridge)

Network coprocessor firmware built with **ESP-IDF**. In keeping with the node's security and timing isolation boundary, the ESP32 has **no direct access to I2C sensors or raw STM32 SRAM** — it validates deterministic 4-byte state frames (`[0xAA][SEQ_ID][STATE_FLAG][CRC8]`) from STM32 `UART4` and forwards only valid frames to the local Mosquitto MQTT broker.

## Hardware Wiring (Default Kconfig Mapping)

| ESP32 DevKit V1 | STM32 Nucleo-F303ZE | Notes |
| :--- | :--- | :--- |
| `GPIO 16` (`UART_NUM_2` RX) | `UART4_TX` | `115200` baud, 8N1 (`[0xAA][SEQ][STATE][CRC8]`) |
| `GPIO 17` (`UART_NUM_2` TX) | `UART4_RX` | Optional return path |
| `GND` | `GND` | Shared ground reference required |

## Configuration & Build

1. Configure Wi-Fi (`Example Connection Configuration`) and broker/UART settings (`Edge AI Gateway Configuration`):
   ```bash
   idf.py menuconfig
   ```
   * `CONFIG_EDGE_AI_MQTT_BROKER_URI` (default `mqtt://broker.local:1883`)
   * `CONFIG_EDGE_AI_MQTT_TOPIC` (default `edge_ai/telemetry` — matches `telemetry_backend/`)
   * `CONFIG_EDGE_AI_UART_BAUD_RATE` (default `115200`)

2. Build, flash, and monitor:
   ```bash
   idf.py build flash monitor
   ```

*(Note: `sdkconfig`, `build/`, and `managed_components/` are excluded via `.gitignore` so local Wi-Fi credentials are never committed.)*
