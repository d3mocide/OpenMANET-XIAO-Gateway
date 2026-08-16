# Pi-side reference

What the XIAO gateway has to interoperate with. Everything the firmware assumes about the other
end of the HaLow link lives here.

Two categories, kept strictly separate: **confirmed** (traceable to OpenMANET source or to a
verified statement) and **still to verify** (needs five minutes on a real node). Cite the source
whenever you move an item from the second list to the first.

- Companion docs: [`ROADMAP.md`](ROADMAP.md) (status and what's next),
  [`HARDWARE.md`](HARDWARE.md) (the XIAO side, and the bring-up runbook)
- **Last updated:** 2026-08-16

## The link, in one paragraph

The Pi runs a real HaLow node *and* the Pi↔XIAO link is itself HaLow. The design assumes the Pi's
HaLow radio presents an **AP**; the XIAO's HaLow radio associates to it as a **station**. Both the
Pi and the XIAO separately run their own local 2.4 GHz Wi-Fi AP for directly-attached clients. So
traffic flows **client → Wi-Fi → XIAO → HaLow → Pi → Wi-Fi → client**, in both directions.

**This "Pi's HaLow radio in AP mode" premise no longer holds out of the box — confirmed 2026-08-16,
see item 0 under "Still to verify".** No current OpenMANET role or setup wizard exposes a HaLow
radio in plain AP mode, which is exactly what a STA-only client radio like the XIAO's needs to
associate to. There's a promising, not-yet-hardware-tested theory for a manual workaround in item 0
— it's a real gap in the ecosystem, worth raising with OpenMANET, not a bug in this firmware.

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
  something to associate to (confirmed 2026-08-06 — but see item 0 under "Still to verify": newer
  reading of OpenMANET's own docs conflicts with this and downgrades it from settled fact to
  open question). Separate from — or possibly sharing a radio with — the 802.11s backbone above.
- **`openmanetd`** (Go daemon on every Pi) handles batman-adv gateway detection/advertisement,
  DHCP server config and static leases, interface/traffic monitoring, a gRPC/HTTP API + React UI,
  and GPS/gpsd integration that **already publishes NMEA and Cursor-on-Target**. CoT is a
  first-class citizen on this mesh already, which is good news for the XIAO's application layer.
  There is no existing "lightweight client/sensor node" mode in `openmanetd` — every current node
  is a full Pi.
- **Same silicon on the HaLow side:** a Pi's WM6108 HaLow HAT and the XIAO's HaLow module are both
  **Morse Micro MM6108**. PHY/MAC behaviour, channel plan and regulatory config line up directly;
  the two ends differ only in host stack (Linux mac80211/hostapd on the Pi vs. Morse's embedded SDK
  on the XIAO). **Correction, 2026-08-16**: an earlier version of this file also named "WM1302" here
  as a second Morse Micro radio on the Pi. That was wrong — WM1302 is a Semtech **SX1302 LoRa**
  gateway module (verified against Seeed/Waveshare's own product pages), unrelated silicon, almost
  certainly there for OpenMANET's separate BLOS/Alfred long-range telemetry path, not the HaLow mesh
  backbone. **A Pi with only a WM6108 HAT has exactly one Morse Micro radio** — there is no second
  HaLow radio to freely dedicate to AP mode while leaving the other on the mesh backbone, unless
  someone has actually fitted two WM6108 HATs (or another dual-HaLow-radio board) to that specific
  Pi. Don't assume a spare HaLow radio exists — check `iw dev` for how many `mm6108`/S1G-band
  interfaces are actually present before planning around one.
- **The module match is exact, not just same-family.** User-confirmed 2026-08-16: this specific
  Pi's HaLow HAT and the XIAO's HaLow module are both literally **Quectel FGH100M-H** — the same
  part, not just "both Morse Micro MM6108" in the abstract. This meaningfully derisks item 3 below:
  the FGH100M-H is the **-H** variant, whose BCF (`bcf_fgh100mhaamd.bin`) only carries real
  calibration for **US** (see `HARDWARE.md` "Regulatory domain" — the non-`-H` FGH100M has separate
  EU/JP BCFs, this part doesn't). So this Pi's regulatory domain isn't merely *supposed* to be US to
  match the XIAO, it's **hardware-incapable of being anything else that actually works** — a non-US
  `country` setting in `openmanetd` would hit the same silent, empty-calibration failure mode
  documented for the XIAO side, not a working alternate region. One less thing to chase if the AP
  workaround in item 0 doesn't pan out for some other reason.
- **ESP32 HaLow cannot do STA-to-STA direct links** (confirmed by Morse's own team). This is why
  the Pi-side AP requirement is not optional.
- **HaLow has no WPA2-PSK.** Verified against the real `morsemicro/halow` SDK source rather than
  its docs: `enum mmwlan_security_type` in `mmwlan.h` defines only `MMWLAN_OPEN`, `MMWLAN_OWE` and
  `MMWLAN_SAE`. The real option set is **open / OWE / SAE**. Early drafts of this project's design
  said "open / WPA2-PSK / WPA3-SAE", which was wrong.
- **openmanetd's own config API can select an encryption mode the XIAO can never use, and won't stop
  you.** Confirmed by reading `openmanet.wifi_config.v1`'s `wifi_config.proto`
  (`github.com/OpenMANET/protobufs`, fetched 2026-08-16): `RadioSettings.encryption` is a
  `WifiEncryption` enum with `SAE`, `PSK2`, `PSK`, `PSK_MIXED`, `NONE` and `OWE` as options - `PSK2`/
  `PSK`/`PSK_MIXED` are all WPA(2)-PSK variants, none of which the XIAO's Morse SDK can associate
  with (see the point above). The API doesn't validate that against the radio's actual PHY, so a Pi
  HaLow radio configured with `PSK2` will simply never be joinable by this firmware - indistinguishable
  from a regulatory-domain or antenna problem from the XIAO's side. **The Pi's HaLow AP radio must be
  set to `SAE`, `OWE`, or `NONE`.**
- **A single HaLow radio on the Pi is one `WifiMode`, not a combination.** Also from
  `wifi_config.proto`: `RadioSettings.mode` is a `WifiMode` enum - `AP` / `Mesh` / `STA` / `Adhoc` /
  `Monitor` - and the message carries separate `ssid` ("SSID (network name)") and `mesh_id` ("Mesh ID
  (only for mesh-mode radios)") fields used depending on which mode is selected. `ListRadios` can
  return more than one physical radio per device (example IDs `radio0`/`radio1`/`radio2` in the
  service definition), so a Pi with two HaLow radios can dedicate one to `Mesh` (the 802.11s backbone
  to other Pis) and the other to `AP` (for XIAO leaf nodes) - but one radio's `RadioSettings` cannot
  be both at once through this API. **If a Pi has only one HaLow radio and it's set to `Mesh` mode,
  there is no AP for the XIAO's STA to associate to at all** - `gwcfg-scan` finds nothing, which is
  the first thing to check before suspecting the regulatory domain or hardware. This sharpens item 4
  below rather than answering it: it shows the *config model* is one-mode-per-radio, not whether a
  given Pi's driver/hardware can run AP+Mesh concurrently on one radio (still unconfirmed).

## Still to verify

Items 1-3 don't block a single-Pi + single-XIAO *build*, and are handled as provisioned config
(NVS) on the XIAO side except item 3 (build-time). **Item 0 is a different kind of unknown**: it's
not a config value to plug in, it's whether the thing this whole project assumes — a HaLow radio on
the Pi running as a plain AP — currently exists as an option at all. Everything else on this list is
moot until item 0 has an answer.

0. **No current OpenMANET role/wizard exposes a HaLow radio in plain AP mode. Confirmed 2026-08-16**
   on a real Pi set up as a mesh gate — zero AP-mode option anywhere in the guided setup for a
   XIAO-class STA-only device to associate to. This is a real gap in the ecosystem, not a local
   misconfiguration, and it's the reason `gwcfg-scan` finds nothing no matter what SSID/key/channel
   is entered. **This is not a batman-adv problem** — batman-adv runs a layer above 802.11
   association, the XIAO deliberately doesn't run it (L3 NAT + relay instead — see `ROADMAP.md`
   "Settled decisions"), and that's an intentional, working choice, not a gap. The break is one
   layer below batman-adv: nothing on the Pi currently beacons an infrastructure AP on the HaLow
   band for the XIAO's STA to find.

   **Working theory for a fix, not yet proven on hardware** — from reading OpenMANET's docs and
   source (`github.com/OpenMANET/openmanetd`, fetched 2026-08-16):
   - The docs' exact wording is *"Bridge mode and the HaLow AP wizard were removed in this
     release. The mesh gate always NATs the mesh into whatever upstream you plug into `eth0`."*
     That's scoped to the **mesh gate role's own guided wizard** specifically (gate = router/NAT to
     an upstream WAN via `eth0`, the opposite direction from what the XIAO needs) — it does not by
     itself prove the underlying radio-mode capability is gone everywhere.
   - `openmanet.wifi_config.v1`'s schema (see "Confirmed" above) has no separate code path for
     S1G/HaLow — `WifiBand` includes `S1G` as just another value alongside `2G`/`5G`/`6G`, and
     `RadioSettings.mode` is one generic `WifiMode` field regardless of band.
   - Reading `openmanetd`'s actual source backs that up: `internal/network/uci_wireless.go` — the
     code that turns a radio's settings into real UCI wireless config (`option mode 'ap'`, etc.) —
     treats `Mode` as a plain passthrough field with **no S1G-specific branch, restriction, or
     rejection** anywhere in it. Nothing found in `internal/network/` or `internal/mgmt/` special-
     cases HaLow band radios to forbid AP mode; the restriction (if it's enforced at all beyond the
     UI) would have to live in a handler this pass didn't reach.
   - **Conclusion to test**: the gap may be in the *guided setup wizard's* UX only, not in
     `openmanetd`'s config pipeline itself. Try setting the HaLow radio's `mode` to `AP` (band
     `S1G`, encryption `SAE`/`OWE`, a real SSID, a channel/bandwidth inside 902–928 MHz) through
     `openmanetd`'s **general radio/Wireless settings** (not the mesh-gate wizard) — the web UI's
     own docs describe a "Wireless tab" with "HaLow + 2.4/5 GHz radio settings" as a real, separate
     panel — or by calling `wifi_config.v1.WifiConfigService/UpdateRadioSettings` directly against
     `openmanetd`'s API (port 8087, gRPC + HTTP/JSON per its README). If `hostapd_s1g` actually
     comes up in AP mode, `gwcfg-scan` should immediately see it — that single test result (works /
     silently reverts / rejected with an error) is the next thing to gather, and it tells us which
     of three very different problems this actually is.
   - **Tradeoff if it works**: a Pi typically has exactly **one** Morse Micro HaLow radio (the
     WM6108 HAT) — see the correction above about WM1302 being unrelated LoRa silicon — so
     dedicating it to AP mode pulls it out of the 802.11s backbone entirely (one radio, one
     `WifiMode`, confirmed above). That's a non-issue for today's single-Pi + single-XIAO scope;
     it becomes exactly item 4 below once a second Pi needs to mesh with this one.
   - **If it doesn't work** — hostapd_s1g rejects AP mode, or openmanetd silently reverts it, or a
     handler this pass didn't find actually does forbid it — then this genuinely has no workaround
     within the current ecosystem, and the right move is raising it with OpenMANET directly: there
     are currently **zero open GitHub issues on `OpenMANET/openmanetd`** mentioning HaLow/AP mode,
     so this would be a first report, not a known/tracked limitation. The ask would be a supported
     way to run a HaLow radio as a leaf-facing AP for STA-only client devices (this project isn't
     the only thing that would want that) — either restoring wizard support for it, or documenting
     the manual API path as supported rather than incidental.

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
   **Largely derisked 2026-08-16** (see the module-match point above): this specific Pi's HaLow
   module is confirmed to also be a Quectel FGH100M-H, the same US-only `-H` part as the XIAO's —
   so it isn't just supposed to match, it's hardware-incapable of a working non-US configuration.
   Still worth a `gwcfg-scan`/`morse-regdb` sanity check once the AP-mode question (item 0) is
   settled, but no longer a likely independent cause of a failed scan on this pairing.

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
   block a single-Pi build. **Narrower and more urgent than it looks**: `openmanetd`'s own
   `wifi_config.proto` (see above) models one radio as one `WifiMode`, so even a *single*-Pi build
   needs that Pi's HaLow radio actually set to `AP` mode with `SAE`/`OWE`/`NONE` encryption - not
   `Mesh` mode, and not a `PSK`/`PSK2` encryption choice the UI will happily let you pick. Check with
   `openmanetd`'s `GetRadioSettings`/dashboard, or `iw dev` / `hostapd_cli -i <ifname> status` on the
   Pi directly, before assuming a scan failure is regulatory or hardware.

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
