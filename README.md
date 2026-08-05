# OpenMANET-S3-Client

Firmware for a XIAO ESP32-S3 + Wio-WM6180 (HaLow) node that acts as a mesh-connected access
point for the [OpenMANET](https://github.com/d3mocide) project: phones/tablets/ATAK devices
associate to the XIAO's local 2.4GHz Wi-Fi AP, and the XIAO relays their IP traffic — including
ATAK Cursor-on-Target (CoT) multicast — over a HaLow uplink into the mesh via a Raspberry Pi
node.

```
client (phone/tablet/ATAK) → Wi-Fi → XIAO SoftAP → IP fwd/NAT → HaLow STA → HaLow AP → Pi → mesh
```

See [`docs/DESIGN.md`](docs/DESIGN.md) for the full design (network architecture, hardware
choices, the NAT-vs-routed tradeoff, build order), [`docs/pi_side_reference.md`](docs/pi_side_reference.md)
for what's confirmed vs. still-to-verify about the Pi side of the link, and
[`docs/PROGRESS.md`](docs/PROGRESS.md) for current status and what's next - start there if
you're picking this project back up.

## Status

Initial firmware skeleton per `docs/DESIGN.md` §7/§8. Structurally complete, but two things are
not yet real (see `docs/PROGRESS.md` for the full checklist):

- **HaLow STA uplink** (`main/uplink_halow.c`): the actual `morsemicro/halow` component calls
  are stubbed out (clean-compiling, return `ESP_ERR_NOT_SUPPORTED`) rather than guessed against
  unverified headers — see the comment block at the top of that file for what's expected once
  wired up. Everything downstream (reconnect/backoff, NAT, CoT relay, SoftAP) is written to
  degrade gracefully while this is stubbed, so it can all be bench-tested without HaLow hardware.
- **Pi-side HaLow AP config** (SSID/security mode/channel) is unconfirmed — see
  `docs/pi_side_reference.md`. Nothing is hardcoded; it's all provisioned via NVS with placeholder
  defaults (see `gwcfg-*` console commands below).

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
(open uplink, `xiao-gateway` SoftAP on `192.168.50.0/24`). To provision a real node, connect over
the serial console (`idf.py monitor`) and use:

```
xiao-gw> gwcfg-show
xiao-gw> gwcfg-set-uplink <ssid> <psk|-> <open|psk|sae> [channel]
xiao-gw> gwcfg-set-softap <ssid> <psk|-> [channel]
xiao-gw> gwcfg-set-node <node_id>
xiao-gw> gwcfg-save
```

Reboot after `gwcfg-save` for uplink/SoftAP changes to take effect.

## Repo layout

```
main/
├── app_main.c          entrypoint: wires everything below together
├── gw_config.h          shared config structs (uplink/softap/CoT/node)
├── provisioning.c       NVS-backed config load/save + gwcfg-* console commands
├── uplink_halow.c       HaLow STA uplink, reconnect/backoff (component integration stubbed)
├── downlink_softap.c    local 2.4GHz SoftAP + DHCP for phones/tablets/ATAK devices
├── ip_forward_nat.c     NAPT between the uplink and SoftAP netifs
└── cot_relay.c          ATAK CoT multicast relay (239.2.3.1:6969) between both netifs
docs/
├── DESIGN.md            full design document
├── pi_side_reference.md confirmed vs. open questions about the Pi side of the link
└── PROGRESS.md          current status, build-order checklist, and what's next
```
