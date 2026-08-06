# Pi-side reference

What the XIAO gateway has to interoperate with. Everything the firmware assumes about the other
end of the HaLow link lives here.

Two categories, kept strictly separate: **confirmed** (traceable to OpenMANET source or to a
verified statement) and **still to verify** (needs five minutes on a real node). Cite the source
whenever you move an item from the second list to the first.

- Companion docs: [`ROADMAP.md`](ROADMAP.md) (status and what's next),
  [`HARDWARE.md`](HARDWARE.md) (the XIAO side, and the bring-up runbook)
- **Last updated:** 2026-08-05

## The link, in one paragraph

The Pi runs a real HaLow node *and* the Pi↔XIAO link is itself HaLow. The Pi's HaLow radio
presents an **AP**; the XIAO's HaLow radio associates to it as a **station**. Both the Pi and the
XIAO separately run their own local 2.4 GHz Wi-Fi AP for directly-attached clients. So traffic
flows **client → Wi-Fi → XIAO → HaLow → Pi → Wi-Fi → client**, in both directions.

The XIAO does not participate in `bat0` itself — it only holds an uplink IP into the mesh.

## Confirmed from OpenMANET source

- **Backbone radio (HaLow, external module):** genuine **802.11s mesh point** mode, not a simple
  AP. `MorseMicro/morse-feed` ships `hostapd_s1g` / `wpa_supplicant_s1g` (mesh-capable forks) plus
  `openmanet_mesh11sd`, a daemon (based on upstream `mesh11sd`) that live-tunes 802.11s mesh
  parameters via the mesh interface after it is already up, because many of them cannot be set
  until the interface exists.
- **L2 mesh routing:** `OpenMANET/packages/routing/` contains `batman-adv`, `batctl` and `alfred`.
  This is the standard "batman-adv over 802.11s" pattern: the 802.11s mesh interfaces form `bat0`
  on every Pi, giving one flat, bridged L2 broadcast domain across the whole backbone — which is
  exactly why multicast (ATAK CoT) works across multiple hops without extra plumbing.
- **Client-facing access:** Pi boards' onboard 2.4 GHz Wi-Fi runs **AP mode only** for locally
  attached clients, bridged into `bat0`.
- **The Pi–XIAO link is HaLow**, with the Pi's HaLow radio in **AP mode** so the XIAO's STA has
  something to associate to (confirmed 2026-08-06). Separate from — or possibly sharing a radio
  with — the 802.11s backbone above.
- **`openmanetd`** (Go daemon on every Pi) handles batman-adv gateway detection/advertisement,
  DHCP server config and static leases, interface/traffic monitoring, a gRPC/HTTP API + React UI,
  and GPS/gpsd integration that **already publishes NMEA and Cursor-on-Target**. CoT is a
  first-class citizen on this mesh already, which is good news for the XIAO's application layer.
  There is no existing "lightweight client/sensor node" mode in `openmanetd` — every current node
  is a full Pi.
- **Same silicon on both ends:** the Pi's WM6108/WM1302 radios and the XIAO's HaLow module are
  both **Morse Micro MM6108**. PHY/MAC behaviour, channel plan and regulatory config line up
  directly; the two ends differ only in host stack (Linux mac80211/hostapd on the Pi vs. Morse's
  embedded SDK on the XIAO).
- **ESP32 HaLow cannot do STA-to-STA direct links** (confirmed by Morse's own team). This is why
  the Pi-side AP requirement is not optional.
- **HaLow has no WPA2-PSK.** Verified against the real `morsemicro/halow` SDK source rather than
  its docs: `enum mmwlan_security_type` in `mmwlan.h` defines only `MMWLAN_OPEN`, `MMWLAN_OWE` and
  `MMWLAN_SAE`. The real option set is **open / OWE / SAE**. Early drafts of this project's design
  said "open / WPA2-PSK / WPA3-SAE", which was wrong.

## Still to verify

None of these block a single-Pi + single-XIAO *build*. Items 1, 2 and 4 are handled as provisioned
config (NVS) on the XIAO side, not hardcoded. **Item 3 is the exception** — it is a build-time
setting, so it needs a real answer before the XIAO can associate at all.

Check with `uci show wireless`, `iw dev`, `iw list`, `batctl if` on the Pi.

1. **Security mode** the Pi's HaLow AP runs — must be one of `open` / `owe` / `sae` (see above).
   `gwcfg-set-uplink` on the XIAO takes exactly those three and has to match.
2. **DHCP scope / lease behaviour**: does the Pi the XIAO associates to run the DHCP server itself,
   or does `openmanetd` centralize it elsewhere? Matters once multiple Pi nodes are in play.
   Also worth noting **whether the Pi's DHCP offers a DNS server** — the XIAO copies the uplink's
   DNS into its own clients' leases, so if the Pi offers none, phones behind the XIAO get IP
   connectivity with no name resolution. The firmware logs a specific warning for this case.
3. **Regulatory / country code** (`morse-regdb`) the Pi's HaLow radio uses. **This one has to be
   fixed on the Pi, not the XIAO: the XIAO is US / 902–928 MHz and cannot be built otherwise.** Its
   module is a Quectel FGH100M-H, a 902–928 MHz part whose BCF carries US calibration only — see
   [`HARDWARE.md`](HARDWARE.md) "Regulatory domain". So the Pi's HaLow radio must run **US**.

   **Unlike 1/2/4 this is not NVS-provisioned on the XIAO side** — confirmed by reading
   `mmhalow_init()` in the real SDK: there is no per-connection channel argument in the STA connect
   API at all. The country code is a *build-time* Kconfig value (`CONFIG_HALOW_COUNTRY_CODE`) that
   determines the STA's legal channel list before it ever scans, and `gwcfg-*` cannot change it at
   runtime.

   A mismatch is silent and looks like the AP being down. `gwcfg-scan` prints the region it
   scanned for exactly this reason.
4. **Once a second Pi joins the mesh**: does a Pi's HaLow radio need to run mesh-point (backbone)
   *and* AP (for XIAO nodes) concurrently on one radio (mac80211 multi-vif), or does each XIAO
   always pair to one specific Pi? Check vif-combination support (`iw list`) at that point. Doesn't
   block a single-Pi build.

## What the XIAO does *not* get from the Pi

Worth stating, because it shapes what is and isn't possible:

- **No inbound unicast to a specific client behind the XIAO.** batman-adv is a Layer 2 mesh with no
  concept of routing to an IP subnet that only exists behind a NAT'd leaf. Nothing on the mesh can
  originate a connection *to* a phone on the XIAO's SoftAP. The v2 fix, if it's ever needed, is a
  static route on the gateway Pi — not a firmware change. See [`ROADMAP.md`](ROADMAP.md) for the
  full NAT-vs-routed reasoning.
- **Multicast is the exception**, and it is why the CoT relay exists: because the backbone is one
  flat L2 domain, CoT multicast crosses the mesh without extra plumbing, and the XIAO's relay
  bridges it across the two dissimilar radios that lwIP won't bridge on its own.
