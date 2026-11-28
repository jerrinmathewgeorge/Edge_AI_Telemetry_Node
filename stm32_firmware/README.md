# STM32 Firmware (Phase 2 — In Progress)

Deterministic edge compute modules for the **STM32 Nucleo-F303ZE** (`STM32F303ZETx`, ARM Cortex-M4F). This directory holds the host-verified static-memory C++17 interfaces and mock-DMA unit tests ready to wire into an STM32CubeIDE target project.

## Included Modules (`include/` & `tests/`)

* `telemetry_protocol.h` — Defines the 4-byte air-gap UART frame (`[0xAA][SEQ][STATE][CRC8]`), state flags (`0x00` `NORMAL`, `0x01` `ANOMALY`, `0x02` `CALIBRATING`, `0xFF` `SENSOR_FAULT`), target memory bounds (`2048 B` RAM / `15360 B` Flash), SHT45 (`0x44`) & SGP41 (`0x59`) addresses, and SRAM-only `env_sample_t`.
* `ring_buffer.h` — `edge_ai::StaticRingBuffer<T, Capacity>` backed by compile-time `std::array` storage (zero `malloc`/`free`, zero exceptions).
* `ewma_filter.h` — `edge_ai::EwmaBaselineFilter` tracking slow diurnal temperature/humidity macro-drift in O(1) time and returning clean residuals (`sample - baseline`) with warm-up gating.
* `sensirion_codec.h` — Sensirion CRC-8 (`0x31`, init `0xFF`), SHT45 Temp/RH decoding, and SGP41 RH/T-compensated command builders (`0x26 0x12` conditioning, `0x26 0x19` raw measurement).
* `sensor_pipeline.h` — Hardware-agnostic non-blocking DMA I2C acquisition FSM enforcing 10s SGP41 conditioning, zero `HAL_Delay`, and I2C NACK/timeout/CRC fault recovery (`0xFF`).
* `tests/test_pipeline.cpp` & `tests/test_sensor_pipeline.cpp` — Host `g++ -std=c++17` tests covering Datasheet CRCs, ring buffer wraparound, diurnal drift vs. VOC spike, 4-byte framing, and mock-DMA NACK/timeout recovery.

## Host Test Quick Start

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I include tests/test_pipeline.cpp -o test_pipeline && ./test_pipeline
g++ -std=c++17 -Wall -Wextra -Werror -I include tests/test_sensor_pipeline.cpp -o test_fsm && ./test_fsm
```

## Hardware & Interface Baseline

| Peripheral / Interface | Assignment | Purpose |
| :--- | :--- | :--- |
| **I2C (DMA + Timer Trigger)** | SHT45 (`0x44`), SGP41 (`0x59`) | Non-blocking environmental acquisition (RH/T cross-fed to SGP41) |
| **UART4 (`115200` baud, 8N1)** | ↔ ESP32 `UART_NUM_2` (`GPIO 16/17`) | Air-gapped 4-byte CRC-8 state frame uplink (shared `GND` required) |
| **USART3 (ST-LINK VCP)** | Host PC Serial Monitor | Local bring-up diagnostics without touching the ESP32 link |

## Phase 2 Target Silicon Checklist

1. Generate a clean STM32CubeIDE (`STM32F303ZETx`) project with timer-triggered I2C DMA, `UART4` (`115200`), and `USART3`.
2. Bind `edge_ai::PlatformOps` callbacks to STM32 HAL DMA TX/RX/Error ISRs and verify physical `0x44` & `0x59` ACKs.
3. Plug in the Edge Impulse model (`Float32` vs. `INT8` footprint benchmark, `< 2 KB RAM` / `< 15 KB Flash` target, `DWT->CYCCNT` µs timing, zero `_sbrk`/`malloc` `.map` check).
