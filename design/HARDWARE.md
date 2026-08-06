# Hardware and bring-up

What to buy, how it goes together, and how to prove it works — in that order, because that's the
order you'll do it in.

Read this before flashing. The firmware's radio configuration is board-specific and compiled in;
if the physical hardware differs from what's below, the HaLow radio will not come up and the
failure looks like a software problem.

- Companion docs: [`ROADMAP.md`](ROADMAP.md) (status, what's next),
  [`PI_SIDE.md`](PI_SIDE.md) (the other end of the link)
- **Last updated:** 2026-08-06

---

# Part 1 — The hardware

## Bill of materials

| Part | Exact item | Notes |
|---|---|---|
| MCU | **Seeed Studio XIAO ESP32-S3** | Not the C3/C6/C5 XIAO. The pin map, the BCF file and the SoftAP support all assume S3. |
| HaLow radio | **Seeed XIAO WM6108** (Morse Micro MM6108) | The HaLow expansion board that mates with the XIAO footprint. |
| Antenna | 2.4 GHz antenna for the XIAO (supplied with it) **and** a sub-GHz antenna for the HaLow board | Two separate radios, two separate antennas. |
| Power | USB-C, or a 3.7 V LiPo on the XIAO's battery pads | See "Power" below. |
| Client devices | Any 2.4 GHz Wi-Fi phone/tablet (ATAK EUDs) | Nothing special required. |

### A naming trap worth avoiding

Earlier drafts of these documents called the HaLow module the **"Wio-WM6180"**. That is a
different Seeed HaLow product. The board this firmware is configured for is the **XIAO WM6108**,
which is what Morse Micro's own ESP-IDF component targets in its
`seeed_xiao_esp32s3-seeed_xiao_mm6108` board config — the file whose contents were copied verbatim
into this repo's `sdkconfig.defaults`. If you bought something else with "HaLow" and "Seeed" in the
name, check the pin table below against its documentation before flashing.

## Why this hardware

- **XIAO ESP32-S3** is the only combination Seeed and Morse currently document and test against
  this module (official wiki tutorial, `Seeed-Studio/mm-iot-esp32`, and
  `RobertWCarey/esp-halow-examples` are all S3). Dual-core Xtensa LX7, native 2.4 GHz Wi-Fi for the
  local AP, and enough RAM/flash headroom for lwIP + NAT + a HaLow driver.
- **The HaLow module matches the Pi-side chipset** (MM6108 both ends), so PHY/MAC behaviour,
  channel plan and regulatory config line up directly. SPI is the confirmed host interface.
- **The local client radio is the S3's onboard 2.4 GHz Wi-Fi** in SoftAP mode — free, no extra
  hardware, and every phone/tablet/ATAK device already speaks it.

**On ESP32-C5:** Morse Micro's current SDK (the `morsemicro/halow` component, which supersedes the
archived `mm-iot-esp32` repo) lists C5 as a supported target alongside S3/C3/C6/P4, and now
supports AP mode with WPA3-SAE via a real wpa_supplicant/hostapd port. C5's native Wi-Fi 6, RISC-V
core and lower power are all attractive for a battery node — but nobody has published the C5 +
WM6108 pairing specifically. **Build v1 on S3 (known-good); revisit C5 once that combination has
real mileage.**

## Pin assignment

The HaLow module talks to the ESP32-S3 over SPI plus four control lines. These pins are **not
configurable at runtime** — they are Kconfig values compiled into the binary (`CONFIG_MM_*` in
[`../sdkconfig.defaults`](../sdkconfig.defaults)).

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

**Provenance:** a verbatim copy of
`managed_components/morsemicro__halow/configs/sdkconfig.defaults.seeed_xiao_esp32s3-seeed_xiao_mm6108`
from `morsemicro/halow` v2.11.2-esp32-2, whose header reads `# BOARD: Seeed XIAO ESP32S3` /
`# HAT: Seeed XIAO WM6108`. These are the component author's own values for this exact pairing, not
a guess.

**Cross-checked against the HAT's own board files** — Seeed's
[`WI-FI_HALOW_FGH100M_EXT01_V30.kicad_pcb`][hat-pcb] netlist and
[`..._SCH_20241107.pdf`][hat-sch] (linked from the [product wiki][hat-wiki]). Every populated link
matches the table above, so the Kconfig values are confirmed on paper. What is *not* yet confirmed
is a physical board; step 1 of bring-up is still the check.

[hat-wiki]: https://wiki.seeedstudio.com/getting_started_with_wifi_halow_module_for_xiao/
[hat-pcb]: https://files.seeedstudio.com/wiki/wifi_halow/res/WI-FI_HALOW_FGH100M_EXT01_V30.kicad_pcb
[hat-sch]: https://files.seeedstudio.com/wiki/wifi_halow/res/WI-FI_HALOW_FGH100M_EXT01_V30_SCH_20241107.pdf

Each XIAO pad reaches the module through a series resistor, and **two of those resistors are
marked DNP** (do not populate) on rev V3.0:

| XIAO pad | GPIO | Link | Module pin | Populated? |
|---|---|---|---|---|
| D0 | 1 | R13 `0R` | `RESET_N` (8) | yes |
| D1 | 2 | R10 **`DNP`** | `WAKEUP_IN` (6) | **no** |
| D2 | 3 | R11 `0R` | `SPI_INT` | yes |
| D3 | 4 | R21 `0R` | `SPI_CS` | yes |
| D4 | 5 | R17 **`DNP`** | `BUSY` | **no** |
| D8 | 7 | R14 `22R` | `SPI_CLK` | yes |
| D9 | 8 | R12 `22R` | `SPI_MISO` | yes |
| D10 | 9 | R16 `22R` | `SPI_MOSI` | yes |

The two unpopulated signals are resolved on the HAT instead: `WAKEUP_IN` is pulled to `MOD_3V3`
through R9 (10 K), so the module is permanently awake, and `BUSY` is pulled to GND through R15
(10 K). The firmware's `CONFIG_MM_WAKE=2` and `CONFIG_MM_BUSY=5` therefore drive and sample pins
that connect to nothing.

That is almost certainly deliberate — Seeed chose the pull resistors that make the module behave
without those lines — but it has a bring-up consequence: **GPIO 5 floats on the ESP32-S3 side.** If
the driver samples BUSY and reads noise rather than a steady low, that is the cause, and an internal
pull-down on GPIO 5 is the fix. Check this before suspecting the radio.

**If you use a different HaLow HAT**, do not hand-edit these numbers. Copy the matching file from
that same `configs/` directory in the fetched component (they ship configs for XIAO C3/C6/C5 and
for Waveshare ESP32-P4 boards), and re-check `board.h` for pin collisions.

### Pins this firmware uses beyond the radio

Declared in [`../main/board.h`](../main/board.h), chosen to avoid every GPIO above.

| Function | GPIO | Behaviour |
|---|---|---|
| Status LED | 21 | The XIAO's on-board user LED. Active **low** (the GPIO sinks current). |
| Factory reset | 0 | The XIAO's BOOT button. Held low while pressed. |

## What's left for expansion

The XIAO ESP32-S3 breaks out **11 GPIOs and nothing else** — D0–D10, plus 3V3/GND/5V. Every other
pin on the chip is either committed on-module (flash, octal PSRAM, USB) or simply not brought to a
pad, so 11 is the hard ceiling. GPIO 0 (BOOT) and GPIO 21 (LED) are on-board parts this firmware
already uses; they are not pads you can wire to.

Of the 11, the HaLow HAT claims eight and this firmware claims none of the rest. **Three pads are
free:**

| Pad | GPIO | Default alternate function | Notes |
|---|---|---|---|
| D5 | 6 | I²C SCL, ADC1_CH5 | Plain GPIO, no strapping role. |
| D6 | 43 | UART0 TX | Free — the console is USB Serial/JTAG, not UART0. |
| D7 | 44 | UART0 RX | Same. |

The console point is worth stating precisely, because it is what makes a UART peripheral possible
at all: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` puts logs and the REPL on the native USB peripheral
(GPIO 19/20, not broken out), and ESP-IDF's *secondary* console exists only to cover the opposite
case — `ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG` carries `depends on !ESP_CONSOLE_USB_SERIAL_JTAG`
([`components/esp_system/Kconfig` L296-316, v5.5.1][idf-console]). Nothing in the app or the
bootloader touches UART0. The ROM bootloader still emits its brief startup banner on GPIO 43 before
the app runs; a peripheral on those pins will see that burst once per reset and should ignore it.

[idf-console]: https://github.com/espressif/esp-idf/blob/v5.5.1/components/esp_system/Kconfig#L296

Three pads is enough for one UART device (2 pins) plus one interrupt or enable line — or one I²C
bus, since the ESP32-S3's GPIO matrix will route SDA/SCL to any two pins. It is *not* enough for a
UART device and an I²C bus at the same time.

**You do not have to solder to the XIAO.** The HAT mirrors all 14 XIAO pads onto two 2.54 mm
7-pin rows, CN3 (D0–D6) and CN4 (D7, D8, D9, D10, 3V3, GND, 5V), so the free pads and power are
reachable from the top of the stack.

### The two DNP pads, and why not to count on them

D1 (GPIO 2) and D4 (GPIO 5) are electrically unconnected on rev V3.0 — see the DNP table above —
so in principle the count is five, not three. Treat that as a bonus you may not get:

- The firmware's Kconfig claims both, and they came from Morse Micro's own board config. Reusing
  them means diverging from the vendor's file, which is exactly the kind of edit that looks like
  cleanup to the next person.
- A later HAT revision can populate R10/R17 without renaming the product. Then the pin is shared
  with `WAKEUP_IN` or `BUSY` and the failure is a radio that misbehaves intermittently.

If you want them anyway, verify continuity on the board in front of you first (D1 and D4 pads to
the module) and set `CONFIG_MM_WAKE` / `CONFIG_MM_BUSY` to unused pin numbers rather than leaving
the driver pointed at pins another peripheral is driving.

### Adding a GPS

A GPS is the obvious candidate for the free pins, and it fits — this is the "real GPS" option in
[`ROADMAP.md`](ROADMAP.md)'s self-beacon section.

An **ATGM336H** (or any NMEA-over-UART module: NEO-6M/8M, L76K) needs exactly what's available:

| GPS pin | Connect to | Note |
|---|---|---|
| VCC | 3V3 | 2.7–3.6 V part. ~25 mA acquiring, ~20 mA tracking — negligible against the XIAO's 3V3 rail. |
| GND | GND | |
| TX | D7 / GPIO 44 | GPS → ESP32-S3. The one line you actually need. |
| RX | D6 / GPIO 43 | Only needed to send configuration (baud, rate, constellation). |
| PPS | D5 / GPIO 6 | Optional. Takes the last free pad. |

Software side: any of UART0/1/2 driven onto those pins via the GPIO matrix, 9600 8N1 by default,
parse `$GNRMC`/`$GNGGA`. No new power rail, no level shifting, no bus contention. The real cost is
antenna placement and a third radio's worth of interference to think about, not GPIO.

If PPS isn't needed, D5/GPIO 6 stays free for something else — a battery-sense divider, a second
status LED, a hardware button.

## Assembly

1. **Seat the WM6108 on the XIAO ESP32-S3.** The two boards share the XIAO footprint. Check the
   orientation marking on both — they are symmetrical enough to fit backwards.
2. **Attach both antennas** before powering on.
3. **Power via USB-C** for first bring-up. The XIAO's USB-C port is the chip's native USB
   Serial/JTAG, which is both the flashing interface and the serial console — no separate
   programmer and no external UART bridge involved.

## Antennas

Two radios, not interchangeable:

- **2.4 GHz** — the ESP32-S3's own antenna connector, for the SoftAP that phones join. The XIAO
  ships with a small external antenna; fit it, or local Wi-Fi range will be poor to nonexistent.
- **Sub-GHz (HaLow)** — on the WM6108, for the uplink to the Pi. This is where the range comes
  from, and the one whose placement decides the link budget.

**Never power the HaLow radio without its antenna attached.** Transmitting into an unmatched load
risks the PA. This applies at every stage, including bench testing.

Keep the sub-GHz antenna away from the 2.4 GHz one and off the board's ground plane where
practical. For field use, height beats everything else.

## Power

| Source | Notes |
|---|---|
| USB-C | Flashing and console. Fine for all bench work. |
| LiPo on the XIAO's BAT pads | The XIAO ESP32-S3 has an on-board charger and battery pads underneath. **See the warning below — battery-only may not power the HaLow front end.** |

### The HaLow HAT needs the 5V rail, and the battery doesn't feed it

The FGH100M-H takes **three** supplies, not one: `VBAT` 3.0–3.6 V, `VDD_IO` 1.62–3.6 V, and
**`VDD_FEM` 3.0–5.25 V, typ. 5 V** — the front-end module, i.e. the PA and LNA
([Quectel FGH100M-H specification v1.0.0][fgh-spec], Electrical Features; its ordering code
`FGH100MHAAMD` is also where `CONFIG_MM_BCF_FILE` comes from). On Seeed's HAT, `VDD_FEM` (module
pin 4) is fed from the XIAO's **5V pad** through ferrite FB2, while `VBAT`/`VDD_IO` come from 3V3
through FB1.

The XIAO ESP32-S3's 5V pad is VBUS. **Running on a LiPo alone leaves it at 0 V**, so the radio
core would power up over 3V3 — SPI works, `gwcfg-radio` prints versions — while the PA and LNA
have no supply at all. The symptom is a scan that finds nothing and a link that never associates:
identical to a region mismatch or a missing antenna, and step 1 of bring-up would pass.

This is untested on hardware and is the first thing to check before trusting a battery build. If it
proves out, the options are a 5 V boost into the 5V pad, or lifting FB2 and feeding `MOD_5V` from
3V3 — legal against the 3.0 V minimum, at the cost of PA headroom. Do not simply bridge 3V3 to the
XIAO's 5V pad: that back-feeds the board's charger and regulator input.

[fgh-spec]: https://files.seeedstudio.com/wiki/wifi_halow/res/Quectel_FGH100M-H_Short-Range_Module_Specification_V1.0.0_Preliminary_20241018.pdf

Budget for the HaLow radio drawing meaningfully more than a bare XIAO, particularly while
transmitting. The XIAO's 3V3 regulator is good for roughly 700 mA total, which is the ceiling any
added peripheral shares with the radio. The firmware currently does **no** power management at all — no light sleep, no duty
cycling, no battery voltage sensing. See [`ROADMAP.md`](ROADMAP.md) for what a power pass would
involve. Irrelevant for a first hardware test; not irrelevant for a deployment.

## Regulatory domain

The HaLow channel plan is a **build-time** setting (`CONFIG_HALOW_COUNTRY_CODE`), not something
`gwcfg-*` or the web UI can change. It must match the domain the Pi's HaLow radio runs, or the two
radios will never find each other even though both work perfectly.

- Local from-source builds default to **US** — a working fallback, not a recommendation.
- CI builds one firmware per region; the web flasher's region picker is where the choice is
  actually made, per user, at flash time.
- Buildable regions are exactly the nine Morse Micro's `mmregdb` ships channel tables for:
  **US, CA, EU, GB, AU, NZ, JP, KR, IN**.

A mismatch here is silent: the radio comes up, the scan returns nothing, everything looks broken.
`gwcfg-scan` prints the region it scanned precisely so this can be told apart from an AP that is
genuinely down.

---

# Part 2 — Bring-up runbook

The first time this firmware meets real hardware: what to run, what a pass looks like, and how to
tell two identical-looking failures apart.

The checklist in [`ROADMAP.md`](ROADMAP.md) tracks *whether* each step has passed. This is *how*.
**The step numbers are the same in both documents** — one list, two views of it. Keep them that way
when either changes; a "step 5 passed" note that means different things in different files is worse
than no note.

**Prerequisites:** the node assembled as above with both antennas fitted, and a Pi running its
HaLow radio in AP mode ([`PI_SIDE.md`](PI_SIDE.md)).

## The instruments you have

| Instrument | Reach for it when | Needs |
|---|---|---|
| **Status LED** | You're looking at the node and want to know how far it got | Nothing |
| **Web UI** at `http://172.16.50.1/` | You want detail: state, RSSI, IPs, client count, logs, scan | A phone/laptop on the node's Wi-Fi |
| **Serial console** (`idf.py monitor`) | The SoftAP itself isn't working, or you want a scan table | USB-C cable |

### LED patterns

| Pattern | Meaning | Go to |
|---|---|---|
| Fast triple-blink, repeating | Radio never initialized | Step 1 |
| Slow blink (1 Hz) | Radio up, searching / not associated | Step 2 |
| Double-blink | Associated, but no DHCP lease | Step 3 |
| Solid on | Uplink up — associated and leased | Step 4 |

All three instruments read the same link state, so they never disagree.

## Step 0 — Confirm the Pi side first

Five minutes on the Pi, before touching the XIAO. Everything downstream assumes these answers; the
full list and the commands are in [`PI_SIDE.md`](PI_SIDE.md). Write down:

1. **SSID** of the Pi's HaLow AP.
2. **Security mode** — `open` / `owe` / `sae` only.
3. **Country / regulatory domain** — decides which firmware image you flash, and cannot be changed
   afterwards without reflashing.
4. **Whether the Pi hands out DHCP leases** on that interface, and from what pool.

## Step 1 — Radio is alive

Flash the firmware for the region from step 0, using the web flasher or `idf.py flash`. Attach the
serial console.

```
xiao-gw> gwcfg-radio
```

**Pass:** BCF API version, BCF board description, firmware and morselib version numbers print. This
also runs automatically at every boot, so it's in the log even if you didn't ask.

That output means host↔MM6108 SPI communication works — which rules out wiring, pin config, BCF
file and chip selection all at once. It is the single most valuable check here, because every one
of those failures otherwise presents identically as "it never associates".

**Fail — nothing prints, or the boot log shows `HaLow uplink init failed`:**

The firmware deliberately does *not* start its reconnect loop in this state; it says so once and
leaves the SoftAP, web UI and console running so you can work. The LED triple-blinks.

Check, in order: the HaLow board is fully seated and the right way round; `CONFIG_MM_*` matches
your actual HAT; `CONFIG_MM_BCF_FILE` is the right calibration file for it.

## Step 2 — The Pi's AP is visible

```
xiao-gw> gwcfg-scan
```

or press **Scan for HaLow APs** in the web UI.

**Pass:** the Pi's SSID appears with an RSSI and a frequency. Note the RSSI — it's your baseline
for everything after this. Clicking a result in the web UI fills the SSID field, which also
eliminates typos as a cause of later failures.

**Fail — no APs at all.** Two distinct causes that look identical:

- **The AP is down or out of range.** Check the Pi.
- **The AP is on a channel this firmware may not legally use.** The scan only covers this build's
  `CONFIG_HALOW_COUNTRY_CODE` channel list, which both the console and the web UI print alongside
  the empty result for exactly this reason. If it doesn't match step 0, reflash with the right
  region before concluding anything.

Antenna check: if the scan only finds the AP when you're standing next to the Pi, suspect the
sub-GHz antenna before the radio.

## Step 3 — Associate, then get a lease

Set the uplink credentials — web UI, or:

```
xiao-gw> gwcfg-set-uplink <ssid> <psk|-> <open|owe|sae>
xiao-gw> gwcfg-save
```

Reboot. Then watch `gwcfg-status`, the web UI status panel, or the LED.

These are **two separate milestones** and the firmware reports them separately:

| State shown | Meaning | Cause to chase |
|---|---|---|
| `searching` | Not associated | SSID, security mode, region, or RF |
| `associating` | In progress | Transient — if it sticks, credentials |
| `associated, no lease` | 802.11 association succeeded, DHCP did not | **Pi-side DHCP**, not the radio |
| `up` | Associated and leased | Move to step 4 |

Rows 1 and 3 have nothing to do with each other: row 1 is a radio/credentials problem on the XIAO,
row 3 is a DHCP problem on the Pi. A single "connected" boolean would collapse them into one
indistinguishable failure, which is why the firmware doesn't use one.

If it sits at `associated, no lease`, the firmware restarts its DHCP client once, then disconnects
and re-associates. Check the Pi is actually serving that interface.

## Step 4 — Local SoftAP and DHCP

Can be done at any time — it doesn't depend on the uplink — and it's the natural first hardware
test.

1. Join the node's Wi-Fi (`xiao-gateway` / `openmanet` by default).
2. You should get a `172.16.50.x` address.
3. Browse to `http://172.16.50.1/`.

That is the whole of step 4: the SoftAP, its DHCP server and the web UI, with the uplink out of the
picture. Everything from here needs the uplink `up`.

## Step 5 — NAT: outbound reach from a client

A client on the SoftAP (step 4) and the uplink at `up` (step 3). **Test reachability and name
resolution separately** — they fail independently and for unrelated reasons.

1. **Ping something on the mesh by IP.** Proves NAPT and the default route.
2. **Resolve a hostname.** Proves the DNS server the node copies out of its own uplink lease and
   into the SoftAP's DHCP offers.
3. **Confirm mesh-side that translated source addresses actually appear** — a capture on the Pi
   should show the XIAO's uplink address, not `172.16.50.x`. Traffic leaving untranslated is a
   different fault from traffic never leaving, and only the mesh side can tell them apart.

> **If you joined the node's Wi-Fi *before* the uplink came up**, disconnect and rejoin before
> testing DNS. The DNS server is copied from the uplink's own lease and pushed into the SoftAP's
> DHCP server at the moment the uplink comes up; clients already holding a lease keep their old,
> DNS-less one until they renew. Normal, and only affects the first boot of a session.

If 1 works and 2 doesn't, the log says why — look for
`uplink DHCP lease carried no DNS server`, meaning the Pi didn't offer one.

## Step 5a — Multicast over HaLow, in isolation

**Do this before involving the CoT relay.** Multicast over mesh routing is a classic silent-drop
point and fails in exactly the same way a broken relay does — test them separately or you won't be
able to tell which is at fault.

From a host on the mesh, send to `239.2.3.1:6969` and confirm with a plain multicast receiver on
the Pi that group traffic crosses the mesh at all. Only then bring the relay into the picture.

## Step 6 — CoT relay

With the uplink `up`, the web UI's **CoT relay** stat should show the group and port rather than
`Waiting`. Then:

1. ATAK on a phone behind the node should see CoT from the mesh.
2. ATAK elsewhere on the mesh should see the phone.

If the relay says it started and nothing flows, step 5a tells you whether to look at the relay or
at the mesh.

## Step 7 is not a bench step

The checklist's last entry — web UI authentication — is development work, not something you run on
the node. It's tracked alongside the bring-up steps because it gates shipping and OTA, not because
it belongs in this runbook. See [`ROADMAP.md`](ROADMAP.md).

## When something goes wrong and nobody was watching

- **`GET /api/log`** (the web UI's *Device log* panel) — the last few KB of log, held in RAM. What
  the serial console would have shown, without a cable.
- **Core dumps** — a panic writes a backtrace to the flash coredump partition. Read it back with
  `idf.py coredump-info`.

## Recovery

**Config wrong and the SoftAP unreachable?** Hold the **BOOT button for 5 seconds** while the node
is running. The LED switches to a fast blink after ~1.5 s to acknowledge; release before 5 s to
cancel. At 5 s the config returns to defaults and the node reboots.

This is a *runtime* hold, not hold-during-power-on: holding BOOT at reset puts the chip into the
ROM bootloader instead of running the firmware.

**Everything else:** the USB console is always there, and `gwcfg-reset` + `gwcfg-save` does the
same job.

## Recording results

Tick the checklist in [`ROADMAP.md`](ROADMAP.md) as steps pass, and move anything learned about the
Pi into [`PI_SIDE.md`](PI_SIDE.md)'s confirmed section — those open items are what block anyone
else from repeating this.
