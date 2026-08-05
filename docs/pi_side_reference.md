# Pi-side reference

What the XIAO gateway firmware needs to interoperate with, extracted from
[`DESIGN.md`](../DESIGN.md) §2/§6 so it doesn't get lost in a longer
document. Update this file (and cite the source) whenever one of the open
items below gets confirmed against a real node.

## Confirmed from OpenMANET source

- The mesh backbone is 802.11s **mesh point** mode (`hostapd_s1g` /
  `wpa_supplicant_s1g` from `MorseMicro/morse-feed`, tuned at runtime by
  `openmanet_mesh11sd`) with `batman-adv` on top, forming one flat bridged
  L2 domain (`bat0`) across all Pi nodes — this is why multicast CoT works
  across hops without extra plumbing.
- Pi boards' onboard 2.4GHz Wi-Fi runs **AP mode only** for locally
  attached clients, bridged into `bat0`.
- **The Pi–XIAO link is itself a HaLow link**, separate from (or possibly
  sharing a radio with) the 802.11s backbone: the Pi's HaLow radio runs
  **AP mode** so the XIAO's HaLow STA can associate to it. Both the Pi and
  the XIAO independently run their own local Wi-Fi AP for directly
  attached clients — the XIAO does not participate in `bat0` itself, it
  only has an uplink IP into the mesh via HaLow.
- `openmanetd` (Go daemon on every Pi) handles batman-adv gateway
  detection/advertisement, DHCP server config + static leases,
  interface/traffic monitoring, a gRPC/HTTP API + React UI, and GPS/gpsd
  integration that already publishes NMEA and CoT. CoT is a first-class
  citizen on this mesh already.
- The Pi's HaLow radio (WM6108/WM1302) and the XIAO's HaLow module
  (WM6180) are the same silicon, Morse Micro MM6108 — PHY/MAC/channel
  plan/regulatory config should line up directly between the two.
- ESP32 HaLow cannot currently do STA-to-STA direct links (confirmed by
  Morse's own team) — the XIAO's HaLow radio must associate to a real
  HaLow AP, which is why the Pi-side AP requirement above isn't optional.

## Still needs 5 minutes on a real node

None of these block a single-Pi + single-XIAO v1 build — `uplink_halow.c`
is written to take these as provisioned config (NVS), not hardcoded
values, specifically so firmware work isn't blocked on this. Check with
`uci show wireless`, `iw dev`, `batctl if` on the Pi:

1. **Security mode** the Pi's HaLow AP runs (open / WPA2-PSK / WPA3-SAE) —
   `gwcfg-set-uplink` on the XIAO needs to match whatever the real value
   is. `morsemicro/halow` supports SAE, so this is a config value, not a
   capability gap.
2. **DHCP scope/lease behavior**: does the Pi the XIAO associates to run
   the DHCP server itself, or does `openmanetd` centralize it elsewhere?
   Matters once multiple Pi nodes are in play.
3. **Regulatory/channel plan** (`morse-regdb` country/channel) the Pi's
   HaLow radio uses, so the XIAO's STA config matches instead of scanning
   blind.
4. **Once a second Pi joins the mesh**: does a Pi's HaLow radio need to run
   mesh-point (backbone) *and* AP (for XIAO nodes) concurrently on one
   radio (mac80211 multi-vif), or does each XIAO always pair to one
   specific Pi? Check vif-combination support (`iw list`) at that point.
