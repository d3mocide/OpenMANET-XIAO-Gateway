# Hardware reference

What to buy, how it goes together, and how to tell it's right before you trust the firmware.

Read this before flashing. The firmware's radio configuration is board-specific and baked in at
build time — if the physical hardware differs from what's below, the HaLow radio will not come up
and the failure looks like a software problem.

- **Last updated:** 2026-08-05
- Companion docs: [`BRINGUP.md`](BRINGUP.md) (what to do once it's assembled),
  [`DESIGN.md`](DESIGN.md) (why this hardware), [`PROGRESS.md`](PROGRESS.md) (project status)

## Bill of materials

| Part | Exact item | Notes |
|---|---|---|
| MCU | **Seeed Studio XIAO ESP32-S3** | Not the C3/C6/C5 XIAO. The pin map, the BCF file and the SoftAP support in this firmware all assume S3. |
| HaLow radio | **Seeed XIAO WM6108** (Morse Micro MM6108) | The HaLow expansion board that mates with the XIAO footprint. |
| Antenna | 2.4 GHz antenna for the XIAO (supplied with it) **and** a sub-GHz antenna for the HaLow board | Two separate radios, two separate antennas. See "Antennas" below. |
| Power | USB-C, or a 3.7 V LiPo on the XIAO's battery pads | See "Power" below. |
| Client devices | Any 2.4 GHz Wi-Fi phone/tablet (ATAK EUDs) | Nothing special required. |

### A naming trap worth avoiding

Earlier drafts of these documents called the HaLow module the **"Wio-WM6180"**. That is a
different Seeed HaLow product. The board this firmware is configured for is the **XIAO WM6108**,
which is what Morse Micro's own ESP-IDF component targets in its
`seeed_xiao_esp32s3-seeed_xiao_mm6108` board config — the file whose contents were copied verbatim
into this repo's `sdkconfig.defaults`. If you bought something else with "HaLow" and "Seeed" in the
name, check the pin table below against its documentation before flashing.

## Board pairing and pin assignment

The HaLow module talks to the ESP32-S3 over SPI plus four control lines. These pins are **not
configurable at runtime** — they are Kconfig values compiled into the binary
(`CONFIG_MM_*` in [`../sdkconfig.defaults`](../sdkconfig.defaults)).

| Function | Kconfig | GPIO |
|---|---|---|
| Reset | `CONFIG_MM_RESET_N` | 1 |
| Wake | `CONFIG_MM_WAKE` | 2 |
| IRQ | `CONFIG_MM_SPI_IRQ` | 3 |
| SPI CS | `CONFIG_MM_SPI_CS` | 4 |
| Busy | `CONFIG_MM_BUSY` | 5 |
| SPI SCK | `CONFIG_MM_SPI_SCK` | 7 |
| SPI MISO | `CONFIG_MM_SPI_MISO` | 8 |
| SPI MOSI | `CONFIG_MM_SPI_MOSI` | 9 |

Plus the chip and board-calibration file:

| Setting | Value |
|---|---|
| `CONFIG_MM_CHIP_MM6108` | `y` |
| `CONFIG_MM_BCF_FILE` | `bcf_fgh100mhaamd.bin` |

**Provenance:** these are a verbatim copy of
`managed_components/morsemicro__halow/configs/sdkconfig.defaults.seeed_xiao_esp32s3-seeed_xiao_mm6108`
from `morsemicro/halow` v2.11.2-esp32-2, whose header reads `# BOARD: Seeed XIAO ESP32S3` /
`# HAT: Seeed XIAO WM6108`. They are the component author's own values for this exact pairing, not
a guess — but they have still never been verified against a physical board in this project. The
first thing bring-up does is check them (see `BRINGUP.md` step 1).

**If you use a different HaLow HAT**, do not hand-edit these numbers. Copy the matching file from
that same `configs/` directory in the fetched component (they ship configs for XIAO C3/C6/C5 and
for Waveshare ESP32-P4 boards), and re-check `board.h` for pin collisions.

### Pins this firmware uses beyond the radio

Declared in [`../main/board.h`](../main/board.h), chosen to avoid every GPIO in the table above.

| Function | GPIO | Behaviour |
|---|---|---|
| Status LED | 21 | The XIAO's on-board user LED. Active **low** (the GPIO sinks current). |
| Factory reset | 0 | The XIAO's BOOT button. Held low while pressed. |

GPIO 6 and 43/44 (UART) are left free.

## Assembly

1. **Seat the WM6108 on the XIAO ESP32-S3.** The two boards share the XIAO footprint; the HaLow
   board sits on the castellated pads / header depending on which variant you have. Check the
   orientation marking on both boards — they are symmetrical enough to fit backwards.
2. **Attach both antennas** before powering on. See below.
3. **Power via USB-C** for the first bring-up. The USB-C port on the XIAO is the chip's native USB
   Serial/JTAG, which is both the flashing interface and the serial console — no separate
   programmer, and no external UART bridge involved.

### Antennas

There are two radios and they are not interchangeable:

- **2.4 GHz** — the ESP32-S3's own antenna connector, for the SoftAP that phones join. The XIAO
  ships with a small external antenna for this; it must be fitted or the local Wi-Fi range will be
  poor to nonexistent.
- **Sub-GHz (HaLow)** — on the WM6108, for the uplink to the Pi. This is where the range comes
  from, and it is the antenna whose placement matters for the link budget.

**Never power the HaLow radio without its antenna attached.** Transmitting into an unmatched load
risks the PA. This applies at every stage including bench testing.

The sub-GHz antenna also wants to be away from the 2.4 GHz one and away from the board's ground
plane where practical. For field use, height beats everything else.

## Power

| Source | Notes |
|---|---|
| USB-C | Used for flashing and console. Fine for all bench work. |
| LiPo on the XIAO's BAT pads | The XIAO ESP32-S3 has an on-board charger and battery pads on the underside. |

Budget for the HaLow radio drawing meaningfully more than a bare XIAO, particularly while
transmitting. The firmware currently does **no** power management at all — no light sleep, no duty
cycling, no battery voltage sensing. See [`FEATURES.md`](FEATURES.md) for what a power pass would
involve. For a first hardware test this doesn't matter; for a field deployment it will.

## Verifying the hardware is right, before trusting the firmware

The two cheapest checks, in order. Both are described in full in [`BRINGUP.md`](BRINGUP.md).

1. **`gwcfg-radio` on the serial console.** Prints the HaLow module's BCF, firmware and morselib
   versions, read back over SPI. If this prints version numbers, the wiring, the pin config, the
   BCF file and the chip selection are all correct — which eliminates the entire "is it plugged in
   right?" class of failure in one command. The firmware also runs this automatically at boot, so
   it appears in the log without being asked.
2. **`gwcfg-scan`.** Lists HaLow APs the radio can hear. This proves the radio is not just alive
   but receiving, and tells you whether the Pi's AP is on a channel this build can legally use.

If check 1 fails, nothing else in this document matters until it passes — go back to the pin table.

## Regulatory domain

The HaLow channel plan is a **build-time** setting (`CONFIG_HALOW_COUNTRY_CODE`), not something
`gwcfg-*` or the web UI can change. It must match the regulatory domain the Pi's HaLow radio is
running, or the two radios will never find each other even though both are working perfectly.

- Local from-source builds default to **US** (`sdkconfig.defaults`) — a working fallback, not a
  recommendation for any particular deployment.
- CI builds one firmware per region and the web flasher has a region picker; that is where the
  choice is actually made, at flash time, per user.
- Buildable regions are exactly the nine Morse Micro's `mmregdb` ships channel tables for:
  **US, CA, EU, GB, AU, NZ, JP, KR, IN**.

A mismatch here is silent: the radio comes up, the scan returns nothing, and everything looks
broken. `gwcfg-scan` prints the region it scanned precisely so this can be told apart from an AP
that is genuinely down.
