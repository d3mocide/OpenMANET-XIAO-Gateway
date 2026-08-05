# XIAO HaLow Gateway — Design Document (v0.1, discovery draft)

## 1. Purpose

A battery/portable-power node, built on a Seeed XIAO ESP32-S3 + Wio-WM6180 (HaLow) module,
that acts as a **mesh-connected access point**: ordinary Wi-Fi client devices (phones, tablets,
ATAK end-user devices) associate to the XIAO's local 2.4 GHz AP, and the XIAO relays their IP
traffic — including ATAK Cursor-on-Target (CoT) multicast — over a Wi-Fi HaLow uplink into an
OpenMANET mesh anchored by Raspberry Pi nodes.

In short: it's a pocket-sized extension of what a Pi node already does (local Wi-Fi AP + HaLow
radio), shrunk onto an MCU, to put mesh reach into places you don't want to carry a Pi.

This document is a discovery-phase output: it separates what we've **confirmed from
OpenMANET's actual source** (this repo + its `morse` and `openmanet` feeds) from what still
needs to be **verified against a live Pi node** before implementation starts.

## 2. How the existing OpenMANET mesh actually works (confirmed from source)

This matters because the XIAO has to interoperate with it, not invent its own topology.

- **Backbone radio (HaLow, external module, e.g. WM6108/WM1302):** runs genuine **802.11s mesh
  point** mode, not a simple AP. `MorseMicro/morse-feed` ships `hostapd_s1g` /
  `wpa_supplicant_s1g` (mesh-capable forks) plus `openmanet_mesh11sd` — a daemon (based on
  upstream `mesh11sd`) that live-tunes 802.11s mesh parameters via the mesh interface after
  it's already up, because many mesh parameters can't be set until the interface exists.
- **L2 mesh routing:** `OpenMANET/packages/routing/` contains `batman-adv`, `batctl`, and
  `alfred`. This is the standard "batman-adv over 802.11s" pattern (same as e.g. Freifunk
  networks): the 802.11s mesh interfaces form `bat0` on every Pi, giving one flat, bridged L2
  broadcast domain across the whole backbone — which is exactly why multicast (ATAK CoT) works
  across multiple hops without any extra plumbing.
- **Client-facing access today:** per this repo's README, Pi boards' **onboard 2.4 GHz Wi-Fi**
  is used "AP mode only" for client devices, bridged into the batman-adv fabric. This describes
  how a Pi talks to *its own* local clients within the existing multi-Pi backbone.
- **Confirmed directly (2026-08-06) — the actual design for this project:** the Pi runs a real
  HaLow node (participates in the HaLow network) *and* the Pi–XIAO link itself is over HaLow.
  Both the Pi and the XIAO separately run their own local Wi-Fi AP for directly-attached clients.
  So the traffic path is: **client → (Wi-Fi) → XIAO → (HaLow) → Pi → (Wi-Fi) → client**, in both
  directions. This means the Pi's HaLow radio needs to present an **AP** that the XIAO's HaLow
  **STA** can associate to (see §4.1) — separate from, or possibly the same radio as, its role in
  the broader multi-Pi 802.11s backbone described above.
- **Node brain:** `OpenMANET/openmanetd` is a Go daemon running on every full node. Confirmed
  responsibilities: batman-adv gateway detection/advertisement, DHCP server config + static
  lease management, interface/traffic monitoring, a gRPC/HTTP API + React UI, and — notably —
  **GPS/gpsd integration that already publishes NMEA and Cursor-on-Target (CoT)**. There is no
  existing "lightweight client/sensor node" mode in openmanetd; every current node is a full Pi.
  This confirms CoT is already a first-class citizen on this mesh, which is good news for the
  XIAO's application layer (see §5.3).
- **Same silicon on both ends:** the WM6180-for-XIAO module and the Pi's WM6108/WM1302 radios
  are both **Morse Micro MM6108**. PHY/MAC behavior, channel plan, and regulatory config should
  line up directly; the two ends differ only in host stack (Linux mac80211/hostapd on the Pi vs.
  Morse's embedded SDK on the XIAO).

## 3. Hardware

| Component | Choice | Why |
|---|---|---|
| MCU | **XIAO ESP32-S3** | Only combo Seeed/Morse currently document and test against this exact module (official wiki tutorial + `Seeed-Studio/mm-iot-esp32` + `RobertWCarey/esp-halow-examples` are all S3). Dual-core Xtensa LX7, native 2.4 GHz Wi-Fi (used for the local AP), enough RAM/flash headroom for lwIP + NAT + a HaLow driver. |
| HaLow radio | **Wio-WM6180 for XIAO** (MM6108, SPI) | Matches the Pi-side chipset; SPI is the confirmed host interface. |
| Local client radio | **XIAO S3's onboard 2.4 GHz Wi-Fi**, SoftAP mode | Free — no extra hardware. Standard phones/tablets/ATAK devices already speak this. |

**C5 note:** Morse Micro's *current* SDK path — the `morsemicro/halow` ESP Component Registry
package (v2.11.2-esp32-2 as of writing), which supersedes the now-archived `mm-iot-esp32` repo —
lists ESP32-C5 as a supported target alongside S3/C3/C6/P4, and now supports AP mode (not just
STA) with WPA3-SAE via a real wpa_supplicant/hostapd port. That's promising for a v2 (C5's native
Wi-Fi 6 + RISC-V + lower power are attractive for a battery node), but nobody has published the
C5 + WM6180 pairing specifically yet. **Recommendation: build v1 on S3 (known-good), keep C5 as
an explicit follow-up once the SDK/module combination has real mileage.**

## 4. Network architecture

```
 [Phone / Tablet / ATAK device]              [Phone / Tablet / ATAK device]
        │ 2.4 GHz Wi-Fi (XIAO SoftAP)                │ 2.4 GHz Wi-Fi (Pi onboard AP)
        │ DHCP from XIAO, e.g. 192.168.50.0/24        │ DHCP from openmanetd, mesh subnet
        ▼                                             ▼
 ┌─────────────────────────────┐            ┌───────────────────────────────┐
 │        XIAO ESP32-S3         │            │              Pi                │
 │ ┌─────────┐   ┌────────────┐ │            │ ┌────────────┐  ┌────────────┐ │
 │ │ SoftAP   │IP │ HaLow STA  │ │            │ │ onboard AP │  │ HaLow radio │ │
 │ │ (esp_wifi)◄─►│(morsemicro/│ │            │ │ (bridged   │  │ (hostapd_s1g│ │
 │ │ netif    │fwd│ halow,     │ │            │ │  into bat0)│  │  AP mode,   │ │
 │ └─────────┘   │ WM6180)    │ │            │ └────────────┘  │  see §4.1)  │ │
 │                └─────┬──────┘ │            │                 └──────┬──────┘ │
 └──────────────────────┼────────┘            └────────────────────────┼────────┘
                         │  HaLow (sub-GHz), STA → AP association,      │
                         │  DHCP lease from openmanetd's pool           │
                         └───────────────────────────────────────────┬─┘
                                                                      ▼
                              new bat0 (802.11s mesh + batman-adv, flat L2 —
                                    if/when more than one Pi is deployed)
                                                                      │
                                              [ openmanetd: DHCP, CoT/gpsd, gateway ]
```

Traffic flows both directions: a client behind the XIAO's SoftAP can reach a client behind the
Pi's onboard AP (and vice versa) via the HaLow link — subject to the NAT-vs-routed tradeoff in
§4.3, since the Pi's local clients sit directly on the flat mesh subnet (bridged) while the
XIAO's local clients sit behind the XIAO's IP forwarding (it can't bridge across dissimilar
radios the way batman-adv does).

### 4.1 Uplink (XIAO → Pi)

- HaLow radio joins in **plain station mode**. Morse's own team has confirmed ESP32 HaLow
  cannot currently do STA-to-STA direct links — **a real HaLow AP is required** on the other end.
- **Confirmed design: the Pi's HaLow radio runs in AP mode** (via `hostapd_s1g`, already part of
  `morse-feed`) so the XIAO's HaLow STA has something to associate to. For a single Pi + one or
  more XIAO nodes, this is a straightforward STA↔AP relationship — no exotic capability needed.
- **One thing to confirm once more than one Pi is in play:** if a given Pi is *also* meant to be
  an 802.11s peer with other Pi nodes on the same HaLow radio (the multi-Pi backbone from §2),
  that radio needs to run mesh-point *and* AP concurrently (mac80211 multi-vif) — worth checking
  (`iw dev`, `iw list` for vif combinations) once the topology grows past one Pi. Doesn't block a
  single-Pi + XIAO build.
- Get an address via DHCP from the pool `openmanetd` manages on that Pi — since the Pi's own
  local clients are bridged straight into the same fabric, the XIAO's uplink IP should be
  reachable from the Pi's side without extra plumbing.
- Security: match whatever the Pi's HaLow AP is configured for (open / WPA2-PSK / WPA3-SAE) —
  the new `morsemicro/halow` component supports SAE, so this isn't a capability gap, just a
  config value to confirm against the real node.
  **Correction (verified against the real morsemicro/halow SDK source, not just its docs):**
  HaLow (802.11ah) has no WPA2-PSK mode at all - `enum mmwlan_security_type` in the SDK's
  `mmwlan.h` only defines `MMWLAN_OPEN`, `MMWLAN_OWE`, and `MMWLAN_SAE`. The real option set for
  §6.3 is **open / OWE / SAE**, not WPA2-PSK - see `design/pi_side_reference.md`.

### 4.2 Downlink (XIAO → phones)

- XIAO SoftAP on 2.4 GHz, its own subnet, its own lightweight DHCP server (built into ESP-IDF's
  `esp_netif`/`lwip` — no custom code needed).
- Because the two radios are physically different PHYs, the XIAO **cannot** transparently
  L2-bridge them the way batman-adv bridges Pi nodes (no batman-adv on FreeRTOS/lwIP). It has to
  do L3 IP forwarding between the two `esp_netif` interfaces.

### 4.3 The NAT-vs-route tradeoff (this is the one to think hardest about)

- **NAT/NAPT** (ESP-IDF has this built in, `CONFIG_LWIP_IPV4_NAPT`, the same mechanism used in
  stock "Wi-Fi repeater" examples): simplest, works immediately, phones get outbound reachability
  to the whole mesh. **Downside:** batman-adv is a *Layer 2* mesh — it has no concept of routing
  to an IP subnet that only exists behind a NAT'd leaf. Nobody elsewhere on the mesh can
  originate a connection *to* a specific phone behind the XIAO.
- **Routed, no NAT:** phones get real mesh-routable-looking addresses, but then something has to
  tell the rest of the mesh how to reach that subnet — batman-adv doesn't propagate L3 routes,
  so this needs either a manually-added static route on the gateway Pi, or a (currently
  nonexistent) dynamic mechanism for leaf nodes to announce a subnet. Real capability, real
  complexity.
- **Multicast/CoT is a separate concern from unicast routing either way**: ATAK situational
  awareness depends on CoT multicast (239.2.3.1:6969) reaching phones bidirectionally. Because
  the XIAO is doing IP forwarding in application code regardless of NAT, it can run an **explicit
  multicast relay** — join the CoT group on both interfaces and re-emit datagrams between them —
  which works whether or not the underlying unicast path is NAT'd. This is the one piece of the
  gateway that has to be hand-written; it isn't something NAPT gives you for free.

**v1 recommendation:** NAT for general IP traffic (simple, ships fast) + a dedicated multicast
relay task for CoT (gets ATAK situational awareness working symmetrically even though general
unicast inbound doesn't). Flag inbound-unicast-to-a-specific-phone as a known v1 limitation;
static routing on the gateway Pi is the natural v2 fix if it's needed.

## 5. XIAO firmware components

1. **HaLow STA uplink** — `morsemicro/halow` ESP-IDF component, station mode, DHCP client.
   Reconnect/backoff logic if the associated Pi node drops off the mesh.
2. **Local SoftAP** — stock `esp_wifi` AP mode on the native 2.4 GHz radio, stock DHCP server.
3. **IP forwarding / NAT** — lwIP NAPT between the two `esp_netif`s (mirrors ESP-IDF's own
   Wi-Fi-repeater reference pattern, just with a HaLow netif standing in for the usual Wi-Fi STA
   uplink).
4. **CoT/multicast relay** — a small FreeRTOS task: join 239.2.3.1:6969 on both interfaces,
   forward datagrams bidirectionally. This is what makes the gateway show up correctly on ATAK
   across the whole mesh, not just locally.
5. **Optional: local telemetry/beacon** — since the XIAO already has an IP presence on the mesh,
   it's a natural place to also emit its own CoT (GPS/battery/status) the same way Pi nodes do
   via gpsd — reuses the same relay path, no separate code needed if the CoT relay is written as
   a generic send/receive primitive rather than pure passthrough.
6. **Provisioning** — SSID/PSK for the uplink, local AP SSID/PSK, and node identity need to be
   configurable without reflashing (NVS-backed config + a small onboard config portal or serial
   CLI is the standard ESP-IDF pattern).

## 6. Open questions to verify against the real Pi (this is the "discovery" ask)

These need five minutes on an actual node (`uci show wireless`, `iw dev`, `batctl if`) rather
than more source spelunking, since the two feeds that would answer them definitively
(`morse-feed`'s `luci-app-morseapwizard`, and the actual `/etc/config/wireless` a deployed node
runs) aren't fully readable through GitHub's web UI at the depth needed:

1. **Confirmed (2026-08-06): the Pi's HaLow radio runs AP mode so the XIAO can associate as a
   station**, and both Pi and XIAO independently run local Wi-Fi APs for their own clients (§2,
   §4.1). No dedicated "gateway node" build-out needed for a single-Pi setup.
2. **If/when a second Pi joins the mesh**, does that Pi's HaLow radio need to run mesh-point
   (backbone) *and* AP (for XIAO nodes) concurrently on one radio, or would XIAO nodes always
   pair to a specific, single Pi? Worth checking mac80211 vif-combination support at that point —
   doesn't block a first single-Pi build.
3. **What security mode does the Pi's HaLow AP run** (open/PSK/SAE — **corrected: the real
   XIAO-side option set is open/OWE/SAE, not PSK; see §4.1 note above**) — just need the real
   value from the node's `/etc/config/wireless`.
4. **DHCP scope/lease behavior**: does the Pi the XIAO associates to run the DHCP server itself,
   or does `openmanetd` centralize it elsewhere (relevant once multiple Pi nodes are involved)?
5. **Regulatory/channel config** the Pi's HaLow radio uses (`morse-regdb` country/channel plan),
   so the XIAO's STA config matches instead of scanning blind.

## 7. Proposed new repo structure

```
xiao-halow-gateway/
├── DESIGN.md                 (this document, refined post-verification)
├── main/
│   ├── app_main.c
│   ├── uplink_halow.c        (STA assoc, DHCP, reconnect)
│   ├── downlink_softap.c     (AP + DHCP server)
│   ├── ip_forward_nat.c      (NAPT setup)
│   ├── cot_relay.c           (multicast relay + optional self-beacon)
│   └── provisioning.c        (NVS config, CLI/portal)
├── components/                (vendored or component-registry pin of morsemicro/halow)
├── CMakeLists.txt / sdkconfig.defaults
└── docs/
    └── pi_side_reference.md  (the confirmed facts from §2, kept for future contributors)
```

## 8. Suggested build order

0. **Confirm the Pi's HaLow radio config**: verify it's running `hostapd_s1g` in AP mode, note
   its SSID/security mode/channel (§6.3, §6.5) so the XIAO's STA config matches.
1. Bring up HaLow STA association against that config (validates §6.1/§6.3 for real).
2. Confirm DHCP lease + reachability to something on the mesh (ping a Pi node, confirm from the
   Pi side you can see the XIAO's lease).
3. Bring up local SoftAP + DHCP in isolation (no uplink yet) — validate phones can join.
4. Wire NAPT between the two interfaces — validate a phone gets outbound mesh/internet reach.
5. Add the CoT multicast relay — validate a phone-side ATAK client sees other mesh CoT traffic,
   and other mesh ATAK clients see the phone.
6. Provisioning/config UX pass.
