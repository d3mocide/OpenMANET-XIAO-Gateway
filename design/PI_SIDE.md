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

**This "Pi's HaLow radio in AP mode" premise is now contested — see item 0 under "Still to
verify".** OpenMANET's own published networking docs, as of a 2026-08-16 read, describe every
HaLow radio as simply living in `10.41.0.0/16` (i.e. a backbone/mesh-point member) and say the
"HaLow AP wizard" was **removed** from the current release. If that holds for the Pi you're
pairing with, there may currently be no supported way to expose a plain infrastructure AP on its
HaLow radio at all — which a STA-only client radio like the XIAO's needs. Confirm on the specific
Pi before trusting anything else in this file.

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

0. **Does any current OpenMANET role still let a HaLow radio run in plain AP mode, at all?**
   Raised 2026-08-16 while diagnosing a XIAO that gets zero results from `gwcfg-scan` against a Pi
   set up as a **mesh gate**. Reading OpenMANET's published docs
   ([`openmanet.github.io/docs/networking`](https://openmanet.github.io/docs/networking)):
   - *"Bridge mode and the HaLow AP wizard were removed in this release. The mesh gate always NATs
     the mesh into whatever upstream you plug into `eth0`."* — "mesh gate" here is a router/NAT
     role for the mesh's **upstream** WAN link (Starlink/LTE/hotel Wi-Fi via `eth0`), which is the
     *opposite* direction from what the XIAO needs — it is not about client-facing HaLow AP at all,
     but the wizard that could apparently once configure one has been removed regardless.
   - *"All HaLow radios live in `10.41.0.0/16`"* — phrased identically for every role, with no
     distinction between "backbone member" and "AP for a leaf STA." Client-facing access is
     described exclusively via a **separate** 2.4/5 GHz radio (`br-ahwlan`) or Ethernet, never via
     HaLow.
   - The `openmanet.wifi_config.v1` protobuf schema (see "Confirmed" above) still defines
     `WIFI_MODE_AP` as an enum value, so the underlying capability may still exist even if the
     guided "wizard" no longer offers it — **this needs checking directly against `openmanetd`'s
     API/dashboard on the actual Pi**, not inferred from the docs prose alone.
   - **If no current role exposes HaLow AP mode**, this project's core premise — a STA-only XIAO
     associating to a Pi's HaLow radio — has no supported counterpart on the Pi today, independent
     of SSID/key/channel/region being right. That's a different, and more serious, problem than
     anything else in this file, and is worth raising with OpenMANET directly rather than assuming
     it's a local misconfiguration.
   - **This is not a batman-adv problem.** batman-adv runs a layer above 802.11s association; the
     XIAO deliberately doesn't run it at all (L3 NAT + relay instead — see `ROADMAP.md` "Settled
     decisions"), and that's an intentional, working design choice, not a gap. The open question
     here is one layer below batman-adv: whether 802.11 association between the XIAO's STA and a
     Pi's HaLow radio is even possible under the current OpenMANET release's supported topology.

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
