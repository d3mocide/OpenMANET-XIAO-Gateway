# OpenMANET-XIAO-Gateway

Firmware for a Seeed XIAO ESP32-S3 + Seeed XIAO WM6108 (HaLow, Morse Micro MM6108) node that acts
as a mesh-connected access
point for the [OpenMANET](https://github.com/d3mocide) project: phones/tablets/ATAK devices
associate to the XIAO's local 2.4GHz Wi-Fi AP, and the XIAO relays their IP traffic — including
ATAK Cursor-on-Target (CoT) multicast — over a HaLow uplink into the mesh via a Raspberry Pi
node.

```
client (phone/tablet/ATAK) → Wi-Fi → XIAO SoftAP → IP fwd/NAT → HaLow STA → HaLow AP → Pi → mesh
```

## Architecture

```
 [Phone / Tablet / ATAK device]              [Phone / Tablet / ATAK device]
        │ 2.4 GHz Wi-Fi (XIAO SoftAP)                │ 2.4 GHz Wi-Fi (Pi onboard AP)
        │ DHCP from XIAO, 172.16.50.0/24             │ DHCP from openmanetd, mesh subnet
        ▼                                             ▼
 ┌─────────────────────────────┐            ┌───────────────────────────────┐
 │        XIAO ESP32-S3         │            │              Pi                │
 │ ┌─────────┐   ┌────────────┐ │            │ ┌────────────┐  ┌────────────┐ │
 │ │ SoftAP   │IP │ HaLow STA  │ │            │ │ onboard AP │  │ HaLow radio │ │
 │ │ (esp_wifi)◄─►│(morsemicro/│ │            │ │ (bridged   │  │ (hostapd_s1g│ │
 │ │ netif    │fwd│ halow,     │ │            │ │  into bat0)│  │  AP mode)   │ │
 │ └─────────┘   │ WM6108)    │ │            │ └────────────┘  │             │ │
 │                └─────┬──────┘ │            │                 └──────┬──────┘ │
 └──────────────────────┼────────┘            └────────────────────────┼────────┘
                         │  HaLow (sub-GHz), STA → AP association,      │
                         │  DHCP lease from openmanetd's pool           │
                         └───────────────────────────────────────────┬─┘
                                                                      ▼
                                bat0 (802.11s mesh + batman-adv, flat L2 —
                                    if/when more than one Pi is deployed)
                                                                      │
                                              [ openmanetd: DHCP, CoT/gpsd, gateway ]
```

Traffic flows both directions: a client behind the XIAO's SoftAP can reach a client behind the Pi's
onboard AP and vice versa. The two ends are asymmetric, though — the Pi's clients sit directly on
the flat mesh subnet (bridged into `bat0`), while the XIAO's sit behind **NAT**, because the XIAO
can't L2-bridge two dissimilar radios the way batman-adv bridges Pi nodes. That's why **CoT
multicast gets an explicit relay** rather than coming for free, and why nothing on the mesh can
originate a connection *to* a specific phone behind a XIAO. Full reasoning in
[`design/ROADMAP.md`](design/ROADMAP.md) → "Settled decisions".

## Documentation

Three documents, deliberately:

| Doc | For |
|---|---|
| [`design/ROADMAP.md`](design/ROADMAP.md) | **Start here.** Status, build-order checklist, what isn't built yet, and the decisions that shouldn't be re-made. |
| [`design/HARDWARE.md`](design/HARDWARE.md) | What to buy, how it goes together, the pin map — and the bring-up runbook for first power-on. |
| [`design/PI_SIDE.md`](design/PI_SIDE.md) | The other end of the link: what's confirmed about the Pi and mesh, and what still needs checking on a real node. |

[`CLAUDE.md`](CLAUDE.md) has the working rules for anyone (human or agent) changing the firmware.

## Flashing (no toolchain needed)

`docs/` is a browser-based flasher ([ESP Web Tools](https://esphome.github.io/esp-web-tools/),
Web Serial - Chrome/Edge only) that flashes firmware built automatically by
[`.github/workflows/build-firmware.yml`](.github/workflows/build-firmware.yml) on every push to
`main`. It only flashes firmware with placeholder config; see "Configuring a node" below for
setting real values afterward - nothing is typed into the flasher page itself.

The workflow builds one binary, **US / 902-928 MHz**, with `CONFIG_HALOW_COUNTRY_CODE="US"` baked in
from `sdkconfig.defaults` (a build-time value the device can't be told at runtime). There is no
region picker, because there is nothing to pick: the XIAO HaLow board carries a Quectel
**FGH100M-H**, a 902-928 MHz part whose board-calibration file - `bcf_fgh100mhaamd.bin`, the one
`CONFIG_MM_BCF_FILE` pins - is named "FGH100M-H (US)" in Morse Micro's own `morse-firmware`
manifest, and Seeed document the board as North America only.

This repo used to build nine regions off `mmregdb`'s channel tables. That was checking the channel
plan without checking the radio or the calibration data behind it: four of those regions have
channels outside the module's band (EU and IN entirely, GB and KR partly), two have no BCF entry at
all (CA, GB), and only US ships real calibration data. Every one of those failures is silent. See
`design/HARDWARE.md` "Regulatory domain" for the evidence and the per-region table.

**One-time setup this repo still needs**: GitHub Pages must be enabled with source "GitHub
Actions" (repo Settings → Pages) before the workflow's deploy step will succeed - it isn't
something a push can turn on by itself.

## Status

`idf.py build` passes end-to-end against ESP-IDF v5.5.1 and the real `morsemicro/halow`
component (verified in CI-less form by actually running the build, not just reading code -
see `design/ROADMAP.md` for current status).
**Not yet done, and real-hardware-only from here:**

- **Pi-side HaLow AP config** (SSID/security mode) is unconfirmed — see
  `design/PI_SIDE.md`. Nothing is hardcoded; it's all provisioned via NVS with placeholder
  defaults (see `gwcfg-*` console commands below). `gwcfg-scan` (or the web UI's scan button) will
  tell you what the Pi is actually advertising.
- **`CONFIG_HALOW_COUNTRY_CODE`** is fixed at `"US"` - the only domain this module's 902-928 MHz
  front end and its BCF support - so the Pi's HaLow radio has to be on US too. It's a build-time
  Kconfig value, not something `gwcfg-*` can set at runtime.
- Nothing has been flashed or run on physical hardware - a compiling build isn't a working
  radio link. Association/DHCP/NAT/CoT-relay behavior on real Pi + XIAO hardware is still
  unverified (`design/HARDWARE.md` Part 2 walks through proving each one).
- **No web UI authentication and no OTA delivery.** Both are designed, neither is built, and the
  first blocks the second - see `design/ROADMAP.md`.

## Building

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)
v5.4.4+ (`morsemicro/halow`'s own minimum) with the ESP32-S3 target.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

The `morsemicro/halow` component is pulled automatically from the ESP Component Registry via
`main/idf_component.yml` on first build.

## Configuring a node

Config (uplink SSID/PSK/security, local SoftAP SSID/PSK/subnet, node id, CoT multicast
group/port) is stored in NVS, not hardcoded. On first boot it falls back to placeholder defaults
(open uplink, `xiao-gateway` SoftAP on `172.16.50.0/24`). Two ways to change it, both writing to
the same config - use whichever's convenient:

**Web UI** (no cable needed): connect to the device's own Wi-Fi (`xiao-gateway` /
`openmanet` by default), then browse to `http://172.16.50.1/`. Passwords are never shown back to
you - leave a password field blank to keep its current value.

**Serial console** (`idf.py monitor`, or any terminal at the same USB-Serial-JTAG port):

```
xiao-gw> gwcfg-show
xiao-gw> gwcfg-set-uplink <ssid> <psk|-> <open|owe|sae>
xiao-gw> gwcfg-set-softap <ssid> <psk|-> [channel]
xiao-gw> gwcfg-set-node <node_id>
xiao-gw> gwcfg-save
```

(HaLow has no WPA2-PSK mode - only open, OWE, or SAE. There's no uplink channel option either:
HaLow picks a channel from the regulatory domain set by `CONFIG_HALOW_COUNTRY_CODE`, a build-time
Kconfig value, not something set here or in the web UI.)

Reboot after saving (either transport) for uplink/SoftAP changes to take effect.

## Diagnosing a node

The bring-up instruments, in the order you'd reach for them - full procedure in
[`design/HARDWARE.md`](design/HARDWARE.md).

**The on-board LED**, which needs neither a cable nor a phone:

| Pattern | Meaning |
|---|---|
| Fast triple-blink | HaLow radio never initialized - check wiring/pins/BCF |
| Slow blink (1 Hz) | Radio up, not associated |
| Double-blink | Associated, but no DHCP lease |
| Solid | Uplink up |

**The web UI's status panel** reports the same link state in words, plus uplink RSSI, both
interfaces' IPs, SoftAP client count, whether the CoT relay started, uptime, free heap and the
baked-in region. It also has a **scan** button and a **device log** view.

**Serial console**, when the SoftAP itself isn't cooperating:

```
xiao-gw> gwcfg-status    # link state, RSSI, IPs, relay state, region
xiao-gw> gwcfg-scan      # which HaLow APs are audible, and on what channel
xiao-gw> gwcfg-radio     # HaLow BCF/firmware versions - proves SPI to the module works
```

**Forgot the SoftAP password, or otherwise locked out?** Hold the **BOOT button for 5 seconds**
while the node is running (the LED acknowledges after ~1.5s; release to cancel). Config returns to
defaults and the node reboots. Note this is a runtime hold - holding BOOT at power-on puts the chip
in the ROM bootloader instead.

## Repo layout

```
main/
├── app_main.c          entrypoint: wires everything below together
├── board.h              XIAO pin assignments (status LED, BOOT button) + HaLow pin collisions
├── gw_config.h          shared config structs (uplink/softap/CoT/node)
├── provisioning.c       NVS-backed config load/save + gwcfg-* console commands
├── uplink_halow.c       HaLow STA uplink via morsemicro/halow, reconnect/backoff, scan, RSSI
├── downlink_softap.c    local 2.4GHz SoftAP + DHCP for phones/tablets/ATAK devices
├── ip_forward_nat.c     DNS propagation + uplink as default route + NAPT on the SoftAP netif
├── cot_relay.c          ATAK CoT multicast relay (239.2.3.1:6969) between both netifs
├── status_led.c         on-board LED as a link-state indicator (no cable, no phone needed)
├── factory_reset.c      BOOT-button hold restores default config
├── log_buffer.c         in-RAM ring of recent logs, served at /api/log
├── web_ui.c             on-device HTTP config UI, SoftAP clients only (same NVS config as gwcfg-*)
└── web_ui.html          embedded into the firmware image, not a separate filesystem
partitions.csv           dual-OTA: 2x 3MB app slots + otadata + 64K coredump (app is at 0x20000)
docs/
└── index.html           ESP Web Tools browser flasher page (single US build)
.github/workflows/
└── build-firmware.yml   builds the firmware + deploys docs/ to GitHub Pages on push
design/
├── ROADMAP.md           status, checklist, what's not built yet, settled decisions
├── HARDWARE.md          BOM, pin map, antennas, power + the bring-up runbook
└── PI_SIDE.md           the Pi/mesh end of the link: confirmed vs. still to verify
CLAUDE.md                working rules for anyone changing the firmware
```
