# Hardware and bring-up

What to buy, how it goes together, and how to prove it works — in that order, because that's the
order you'll do it in.

Read this before flashing. The firmware's radio configuration is board-specific and compiled in;
if the physical hardware differs from what's below, the HaLow radio will not come up and the
failure looks like a software problem.

- Companion docs: [`ROADMAP.md`](ROADMAP.md) (status, what's next),
  [`PI_SIDE.md`](PI_SIDE.md) (the other end of the link)
- **Last updated:** 2026-08-05

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
a guess — but they have still never been verified against a physical board. Step 1 of bring-up is
the check.

**If you use a different HaLow HAT**, do not hand-edit these numbers. Copy the matching file from
that same `configs/` directory in the fetched component (they ship configs for XIAO C3/C6/C5 and
for Waveshare ESP32-P4 boards), and re-check `board.h` for pin collisions.

### Pins this firmware uses beyond the radio

Declared in [`../main/board.h`](../main/board.h), chosen to avoid every GPIO above.

| Function | GPIO | Behaviour |
|---|---|---|
| Status LED | 21 | The XIAO's on-board user LED. Active **low** (the GPIO sinks current). |
| Factory reset | 0 | The XIAO's BOOT button. Held low while pressed. |

GPIO 6 and 43/44 (UART) are left free.

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
| LiPo on the XIAO's BAT pads | The XIAO ESP32-S3 has an on-board charger and battery pads underneath. |

Budget for the HaLow radio drawing meaningfully more than a bare XIAO, particularly while
transmitting. The firmware currently does **no** power management at all — no light sleep, no duty
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

## Step 4 — Local Wi-Fi and client connectivity

Can be done at any time — it doesn't depend on the uplink — and it's the natural first hardware
test.

1. Join the node's Wi-Fi (`xiao-gateway` / `openmanet` by default).
2. You should get a `172.16.50.x` address.
3. Browse to `http://172.16.50.1/`.

Then, once the uplink is `up`:

4. **Ping something on the mesh by IP.** Proves NAT and routing.
5. **Resolve a hostname.** Proves the DNS server the node hands out in its DHCP leases works.

> **If you joined the node's Wi-Fi *before* the uplink came up**, disconnect and rejoin before
> testing DNS. The DNS server is copied from the uplink's own lease and pushed into the SoftAP's
> DHCP server at the moment the uplink comes up; clients already holding a lease keep their old,
> DNS-less one until they renew. Normal, and only affects the first boot of a session.

If step 4 works and step 5 doesn't, the log says why — look for
`uplink DHCP lease carried no DNS server`, meaning the Pi didn't offer one.

## Step 4a — Multicast over HaLow, in isolation

**Do this before involving the CoT relay.** Multicast over mesh routing is a classic silent-drop
point and fails in exactly the same way a broken relay does — test them separately or you won't be
able to tell which is at fault.

From a host on the mesh, send to `239.2.3.1:6969` and confirm with a plain multicast receiver on
the Pi that group traffic crosses the mesh at all. Only then bring the relay into the picture.

## Step 5 — CoT relay

With the uplink `up`, the web UI's **CoT relay** stat should show the group and port rather than
`Waiting`. Then:

1. ATAK on a phone behind the node should see CoT from the mesh.
2. ATAK elsewhere on the mesh should see the phone.

If the relay says it started and nothing flows, step 4a tells you whether to look at the relay or
at the mesh.

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
