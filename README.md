# OpenMANET-S3-Client

Firmware for a XIAO ESP32-S3 + Wio-WM6180 (HaLow) node that acts as a mesh-connected access
point for the [OpenMANET](https://github.com/d3mocide) project: phones/tablets/ATAK devices
associate to the XIAO's local 2.4GHz Wi-Fi AP, and the XIAO relays their IP traffic — including
ATAK Cursor-on-Target (CoT) multicast — over a HaLow uplink into the mesh via a Raspberry Pi
node.

```
client (phone/tablet/ATAK) → Wi-Fi → XIAO SoftAP → IP fwd/NAT → HaLow STA → HaLow AP → Pi → mesh
```

See [`design/DESIGN.md`](design/DESIGN.md) for the full design (network architecture, hardware
choices, the NAT-vs-routed tradeoff, build order), [`design/pi_side_reference.md`](design/pi_side_reference.md)
for what's confirmed vs. still-to-verify about the Pi side of the link, and
[`design/PROGRESS.md`](design/PROGRESS.md) for current status and what's next - start there if
you're picking this project back up.

## Flashing (no toolchain needed)

`docs/` is a browser-based flasher ([ESP Web Tools](https://esphome.github.io/esp-web-tools/),
Web Serial - Chrome/Edge only) that flashes firmware built automatically by
[`.github/workflows/build-firmware.yml`](.github/workflows/build-firmware.yml) on every push to
`main`. It only flashes firmware with placeholder config; see "Configuring a node" below for
setting real values afterward - nothing is typed into the flasher page itself.

The workflow builds one firmware binary per region (a build matrix over `country-configs/*.defaults`)
with `CONFIG_HALOW_COUNTRY_CODE` baked in, since that's a build-time value the device can't be told
at runtime. The flasher page has a region picker above the flash button - pick yours before
flashing. Regions currently built: **US, CA, EU, GB, AU, NZ, JP, KR, IN** - exactly the 9 regulatory
domains Morse Micro's `mmregdb` (upstream of `morsemicro/halow`) ships channel data for as of this
writing. That list can't be freely extended to any country: a code with no upstream regdb entry
won't have a real channel plan to build against. Adding a region once Morse Micro ships data for it
is a new `country-configs/<CODE>.defaults` file (one line: `CONFIG_HALOW_COUNTRY_CODE="<CODE>"`)
plus a matching matrix/dropdown entry.

**One-time setup this repo still needs**: GitHub Pages must be enabled with source "GitHub
Actions" (repo Settings → Pages) before the workflow's deploy step will succeed - it isn't
something a push can turn on by itself.

## Status

`idf.py build` passes end-to-end against ESP-IDF v5.5.1 and the real `morsemicro/halow`
component (verified in CI-less form by actually running the build, not just reading code -
see `design/PROGRESS.md` for the full list of what that surfaced and fixed: console peripheral,
IDF version floor, the real HaLow security enum, board pin/BCF/chip config, partition size).
**Not yet done, and real-hardware-only from here:**

- **Pi-side HaLow AP config** (SSID/security mode) is unconfirmed — see
  `design/pi_side_reference.md`. Nothing is hardcoded; it's all provisioned via NVS with placeholder
  defaults (see `gwcfg-*` console commands below).
- **`CONFIG_HALOW_COUNTRY_CODE`** in `sdkconfig.defaults` is still the placeholder `"??"` - only
  matters if you're building from source yourself (see "Building" below); the web flasher's builds
  already bake in a real country code per region. Either way it's a build-time Kconfig value, not
  something `gwcfg-*` can set at runtime.
- Nothing has been flashed or run on physical hardware - a compiling build isn't a working
  radio link. Association/DHCP/NAT/CoT-relay behavior on real Pi + XIAO hardware is still
  unverified (`design/DESIGN.md` §8 steps 0-5).

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
(open uplink, `xiao-gateway` SoftAP on `192.168.50.0/24`). Two ways to change it, both writing to
the same config - use whichever's convenient:

**Web UI** (no cable needed): connect to the device's own Wi-Fi (`xiao-gateway` /
`openmanet` by default), then browse to `http://192.168.50.1/`. Passwords are never shown back to
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

## Repo layout

```
main/
├── app_main.c          entrypoint: wires everything below together
├── gw_config.h          shared config structs (uplink/softap/CoT/node)
├── provisioning.c       NVS-backed config load/save + gwcfg-* console commands
├── uplink_halow.c       HaLow STA uplink via morsemicro/halow, reconnect/backoff
├── downlink_softap.c    local 2.4GHz SoftAP + DHCP for phones/tablets/ATAK devices
├── ip_forward_nat.c     NAPT on the SoftAP netif + uplink as default route
├── cot_relay.c          ATAK CoT multicast relay (239.2.3.1:6969) between both netifs
├── web_ui.c             on-device HTTP config UI, SoftAP clients only (same NVS config as gwcfg-*)
└── web_ui.html          embedded into the firmware image, not a separate filesystem
partitions.csv           custom 3MB app partition (default 1MB is too small) + 64K coredump
docs/
└── index.html           ESP Web Tools browser flasher page, region picker + per-region manifest
country-configs/
└── {US,CA,EU,GB,AU,NZ,JP,KR,IN}.defaults
                        per-region CONFIG_HALOW_COUNTRY_CODE override, layered onto sdkconfig.defaults
.github/workflows/
└── build-firmware.yml   builds one firmware per region + deploys docs/ to GitHub Pages on push
design/
├── DESIGN.md            full design document
├── pi_side_reference.md confirmed vs. open questions about the Pi side of the link
├── TECHNICAL_REVIEW.md  pre-hardware review: findings, fixes, and the open OTA decision
└── PROGRESS.md          current status, build-order checklist, and what's next
```
