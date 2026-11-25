# STM32 Firmware (Phase 2 — In Progress)

Deterministic edge compute modules for the **STM32 Nucleo-F303ZE** (`STM32F303ZETx`, ARM Cortex-M4F). This directory currently holds the Phase 2 static-memory C/C++ interfaces ready to drop into a clean STM32CubeIDE project.

## Included Interfaces (`include/`)

* `telemetry_protocol.h` — Defines the 1-byte air-gap state flags (`0x00` `NORMAL`, `0x01` `ANOMALY`, `0x02` `CALIBRATING`, `0xFF` `SENSOR_FAULT`), memory budget constants (`2048 B` RAM / `15360 B` Flash), SHT45 (`0x44`) & SGP41 (`0x59`) 7-bit I2C addresses, and the SRAM-only `env_sample_t` struct.
* `ring_buffer.h` — `edge_ai::StaticRingBuffer<T, Capacity>` backed by compile-time `std::array` storage (zero `malloc`/`free`, zero exceptions).
* `ewma_filter.h` — `edge_ai::EwmaBaselineFilter` tracking slow diurnal temperature/humidity macro-drift in O(1) time and returning clean residuals (`sample - baseline`) with warm-up gating.

## Hardware & Interface Baseline

| Peripheral / Interface | Assignment | Purpose |
| :--- | :--- | :--- |
| **I2C (DMA + Timer Trigger)** | SHT45 (`0x44`), SGP41 (`0x59`) | Non-blocking environmental acquisition (RH cross-fed to SGP41) |
| **UART4 (`115200` baud, 8N1)** | ↔ ESP32 `UART_NUM_2` (`GPIO 16/17`) | Air-gapped 1-byte state flag uplink (shared `GND` required) |
| **USART3 (ST-LINK VCP)** | Host PC Serial Monitor | Local bring-up diagnostics without touching the ESP32 link |

## Phase 2 Implementation Checklist

1. Generate a clean STM32CubeIDE (`STM32F303ZETx`) project with timer-triggered I2C DMA, `UART4` (`115200`), and `USART3`.
2. Implement non-blocking Sensirion SHT45 + SGP41 I2C transactions (CRC-8 check + SHT45 RH conditioning command feed to SGP41, zero `HAL_Delay` in the acquisition loop).
3. Wire `StaticRingBuffer` → `EwmaBaselineFilter` → statically allocated Edge Impulse Float32 inference (`< 2 KB RAM`, `< 15 KB Flash`), transmitting only `telemetry_state_t` bytes across `UART4`.
