# Roadmap

Where this project is, what's next, and the decisions that shouldn't be re-made. **Start here if
you're picking the project back up.**

- Companion docs: [`HARDWARE.md`](HARDWARE.md) (what to buy, how to build one, how to bring it up),
  [`PI_SIDE.md`](PI_SIDE.md) (the other end of the link)
- Architecture diagram and repo layout: [`../README.md`](../README.md)
- **Last updated:** 2026-08-06

Keep this file current: tick the checklist when a step passes, move an item out of "not built yet"
when it lands, and add to "settled decisions" rather than re-arguing one. Historical detail
belongs in git history, not here — this file describes the present and the plan.

## Status at a glance

`idf.py build` **passes end-to-end** against ESP-IDF v5.5.1 with the real `morsemicro/halow`
component: **zero errors, zero warnings**, binary ~1.67 MB, **44% free** in the 3 MB app
slot on confirmed 8 MB flash. Verified by actually running the build, not by reading code.

**First hardware bring-up is under way.** Steps 1 and 4 have passed on a real XIAO ESP32-S3 +
WM6108: the MM6108 answers over SPI with its version banner, and the SoftAP leases addresses and
serves the web UI. A compiling build proves the code is internally consistent against the real
APIs; it does not prove the radio associates, DHCP completes, or NAT and the CoT relay pass
traffic. Everything remaining in the checklist below is hardware-only from here.

The first thing hardware taught us wasn't in the firmware at all: the HaLow HAT's unpopulated
WAKE/BUSY links make the SDK's *default* power-save setting reboot the node in a loop, which
presents identically to a dead radio. See `CONFIG_HALOW_PS_MODE` under "Settled decisions".

**It also doesn't prove the code means what it says.** Three separate passes have now found bugs in
clean-compiling firmware that would each have failed silently on hardware and looked like a radio
problem:

- the CoT relay matched `IP_PKTINFO`'s `ipi_addr` (the packet's *destination*, always the multicast
  group) instead of `ipi_ifindex`, dropping 100% of traffic while logging success;
- NAPT was enabled on the uplink netif when ESP-IDF requires it on the SoftAP netif;
- SoftAP clients were handed no DNS server, so every hostname lookup failed while raw IP worked.

All three were found by checking this firmware's assumptions against upstream ESP-IDF/lwIP source,
not by re-reading this repo. Assume the same class of error exists elsewhere. See
[`../CLAUDE.md`](../CLAUDE.md) for the working rule this produced.

## What's implemented

| Module | File | Status |
|---|---|---|
| Local SoftAP + DHCP | `main/downlink_softap.c` | 2.4 GHz AP + DHCP server for phones/tablets/ATAK devices. |
| HaLow STA uplink | `main/uplink_halow.c` | STA association, reconnect/backoff, bounded DHCP wait with disconnect-and-retry. Exposes a four-state link state, RSSI, a blocking scan wrapper, and radio version readback. |
| NAT / IP forwarding | `main/ip_forward_nat.c` | All three steps of ESP-IDF's NAT recipe: DNS propagation into the SoftAP's DHCP offers, uplink as default route, NAPT on the downlink. |
| CoT multicast relay | `main/cot_relay.c` | One socket joined to 239.2.3.1:6969 on both netifs, `IP_PKTINFO`/`recvmsg()` for arrival interface, loop prevention via `IP_MULTICAST_LOOP` off + own-source drop. |
| Provisioning | `main/provisioning.c` | NVS config blob (magic + version stamped, validated on load and save) plus `gwcfg-*` console commands over USB Serial/JTAG. |
| Web config UI | `main/web_ui.c` / `.html` | `esp_http_server` + embedded HTML. `GET /api/status`, `GET`/`POST /api/config`, `GET /api/log`, `POST /api/scan`, `POST /api/reboot`. Same NVS config as the console. SoftAP clients only; **no authentication yet**. |
| Status LED | `main/status_led.c` | On-board GPIO21 LED blinks the uplink link state. The only instrument needing neither cable nor phone. |
| Factory reset | `main/factory_reset.c` | 5 s BOOT-button hold restores defaults and reboots; LED acknowledges at 1.5 s. |
| Log ring buffer | `main/log_buffer.c` | `esp_log_set_vprintf` tee into a 6 KB RAM ring, served at `/api/log`. Chains to the previous handler, so serial output is unaffected. |
| App wiring | `main/app_main.c` | Brings up log buffer, LED, factory-reset watcher, SoftAP, console and web UI immediately; NAT + CoT relay once the uplink holds a DHCP lease, retrying on the next reconnect if that fails. Skips the reconnect task entirely if the radio never initialized. |
| Web flasher + CI | `docs/`, `.github/workflows/` | ESP Web Tools page, single US build. GitHub Actions builds `sdkconfig.defaults` unmodified and deploys to Pages; PRs build but don't deploy. |

Everything above compiles clean. **Steps 1 and 4 have now passed on hardware** (see the checklist);
the rest is unrun.

## Build-order checklist

Procedure for each step — what to run, what a pass looks like, how to tell identical-looking
failures apart — is in [`HARDWARE.md`](HARDWARE.md) Part 2. This is the tracker; that is the
runbook. **The step numbers are shared** — if you renumber one, renumber the other, or a recorded
"step 5 passed" stops meaning one thing.

- [ ] **Step 0** — Confirm the Pi's HaLow radio config: AP mode, SSID, security mode, country,
      DHCP behaviour. See [`PI_SIDE.md`](PI_SIDE.md) "Still to verify". The Pi has to be on **US**
      (902–928 MHz) — that's the only domain this hardware can reach.
- [x] **Step 1** — Radio responds over SPI (`gwcfg-radio`). **Passed.** On a Seeed XIAO ESP32-S3 +
      WM6108 (Quectel FGH100M-H), the boot banner reports BCF API 8.0.0, morselib 2.11.2, Morse
      firmware 1.17.8 and chip ID `0x0306`, so the `CONFIG_MM_*` pin map, the BCF file and the chip
      selection are all confirmed against physical hardware. Note the BCF's board description reads
      `mf16858`, *not* a Quectel part number — that is Morse Micro's internal board ID inside
      `bcf_fgh100mhaamd.bin` itself (`.board_desc` in the shipped file), not a mismatch.
- [ ] **Step 2** — The Pi's AP is visible (`gwcfg-scan`). Proves the radio receives, and says
      whether the AP is on a channel this build may legally use.
- [ ] **Step 3** — HaLow STA associates, then gets a DHCP lease. Two separate milestones, reported
      separately (`searching` vs. `associated, no lease`) because they have different causes.
- [x] **Step 4** — Local SoftAP and DHCP validated standalone (phones join, get a lease, reach the
      web UI). **Passed** — `xiao-gateway` comes up on channel 6, a client associates and is leased
      `172.16.50.2`, and the web UI is reachable. Doesn't depend on steps 0–3 — the natural first
      hardware test.
- [ ] **Step 5** — NAT validated: a phone gets outbound mesh reach. **Test IP reachability and
      name resolution separately** — they fail independently, and DNS propagation was missing
      entirely until recently. Confirm translated source addresses actually appear mesh-side.
- [ ] **Step 5a** — **Plain multicast over HaLow validated, before involving the relay.** Confirm
      an IGMP join on the Morse Micro interface receives group traffic and that the Pi-side mesh
      forwards 239.2.3.1 at all. Multicast over mesh routing is a classic silent-drop point and
      fails identically to a broken relay — isolate them or you can't tell which is at fault.
- [ ] **Step 6** — CoT relay validated: ATAK on a phone sees mesh CoT and vice versa.
- [ ] **Step 7** — Web UI authentication. **Not a bench step** — development work, listed here
      because it gates shipping and OTA delivery, so it has no entry in the runbook. Design settled
      below; not started.

## Not built yet

Only one hard dependency exists in this list, and it's a security one: **authentication must land
before any firmware-upload path.** Everything else can be done in any order — let hardware testing
decide. Nothing here should start before the checklist above passes; features built against an
unproven link get debugged twice.

### 1. Web UI authentication — *blocks shipping and OTA*

Association with the SoftAP is currently the only credential. The handlers do refuse requests from
outside the SoftAP subnet, which restores the intended boundary, but that is subnet-based
*authorization*, not authentication — it doesn't defend against a device already on the SoftAP.
Acceptable for bench testing, not for shipping.

Design decisions are settled — see "Settled decisions" below. Implementation notes:

- The existing `reject_if_remote()` gate **stays**. Auth is added to it, not a replacement — the
  two defend against different attackers.
- Interim CSRF guards already exist in `web_ui.c` and also stay: the Host header must name the
  SoftAP's own address (defeats DNS rebinding), and state-changing POSTs must declare
  `Content-Type: application/json` (forces a CORS preflight on cross-origin requests, which the
  server never approves). They close the drive-by-browser attack from a phone on the SoftAP, but
  they are *not* authentication — a hostile client on the SoftAP can still do everything the UI
  can until this item lands. If mDNS/captive portal (item 4) arrives first, the Host check must
  learn those names or the UI becomes unreachable through them.
- `gw_config_t` gains a salt, a hash, and a "password has been set" flag. That's a layout change:
  bump `GW_CONFIG_VERSION`.
- `factory_reset.c` must clear the stored credential too, or a forgotten password survives the one
  recovery path a field-deployed node has.

### 2. OTA update delivery — *blocked on 1*

The partition layout is already dual-OTA (two 3 MB slots + `otadata`, ~1.81 MB unallocated on 8 MB
flash). That half was done immediately because retrofitting a partition table onto deployed units
costs a USB cable per unit — exactly the cost OTA exists to remove.

Delivery is deferred deliberately. An OTA endpoint means anyone who can reach the web UI can
replace the firmware; promoting "can change my config" to "can replace my firmware" on an
unauthenticated endpoint is an escalation, not an increment.

In order:

1. Authentication (item 1). Nothing else ships first.
2. An update path — `esp_https_ota` from a known URL, or an authenticated upload endpoint.
3. `esp_ota_mark_app_valid_cancel_rollback()` in the app, **then** enable
   `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Turning rollback on before that call exists means every
   boot reverts.
4. A real end-to-end test on hardware, **including a deliberately bad image**, before relying on it.

`/api/status` already reports the running partition — meaningless today (always `ota_0`) and the
first thing you'll want once this exists.

### 3. Self-beacon — the node's own CoT

`cot_relay_inject()` is already the generic send primitive, and the relay's own-source drop already
covers injected traffic. What's missing is a producer. Without it the gateway is **invisible on
other people's ATAK screens** — it relays everyone else's situational awareness and contributes
none of its own.

**Scope decision to make.** Pi nodes get position from gpsd; this hardware has no GPS. Two options,
not exclusive:

- **Static/configured position** — a lat/lon in `gw_config_t`, set when the node is placed. Cheap,
  no hardware, genuinely useful for a node on a fixed mast.
- **Real GPS** — a UART module (ATGM336H or similar) on D6/D7 (GPIO 43/44), with D5 (GPIO 6) left
  for PPS. Confirmed to fit; see "What's left for expansion" in [`HARDWARE.md`](HARDWARE.md).
  Better for a mobile node, but new hardware, new config, new failure mode.

Either way the beacon should carry what only this node knows: uplink state, RSSI, client count,
uptime, and battery once item 6 exists. That data is already behind `/api/status`.

CoT is XML; budget a few KB whether you string-build or pull in a formatter.

### 4. Captive portal and mDNS

Today a user must *know* to browse to `172.16.50.1`.

- **mDNS** (`xiao-gw.local`) — small, ESP-IDF ships the component. Works well on iOS/macOS,
  unevenly on Android.
- **Captive portal DNS** — a DNS responder on the SoftAP answering every query with the node's own
  address, so joining the Wi-Fi pops the config page. This is the one that actually solves it, on
  every platform, and it's the standard ESP-IDF pattern.

Worth doing *after* auth: a captive portal landing on a login screen is a much better first-run
experience than one landing on an open config form.

### 5. Multi-group CoT relay

The relay handles exactly one group/port. Some ATAK deployments use additional groups.
`gw_cot_config_t` becomes a short array, `cot_relay_start()` joins each, arrival-interface logic is
unchanged, bump `GW_CONFIG_VERSION`. Do it only if a real deployment needs it — speculative
generality costs config complexity for every user.

### 6. Power management

`DESIGN`-era notes called this "a battery/portable-power node" and the firmware does nothing about
it: no light sleep, no duty cycling, no battery voltage sensing.

**Do not guess before hardware.** Measure first: idle draw with the radio associated but idle; draw
with SoftAP clients attached; whether the MM6108's own power-save modes are usable given the relay
needs to receive multicast promptly. *Then* battery voltage on an ADC pin, reported via
`/api/status` and the beacon, and a considered decision about sleep.

The same argument applies to throughput. If TCP-through-NAT disappoints on hardware, the knobs are
`CONFIG_LWIP_TCP_WND_DEFAULT` / `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` and the Wi-Fi buffer counts — but
measure first. The MM6108 link over SPI is the ceiling (single-digit Mbps at best, far less at
range) and the S3's lwIP NAT path handles that with headroom.

## Settled decisions

Recorded so they aren't relitigated, and so they aren't accidentally undone.

### Authentication

| Decision | Choice | Why |
|---|---|---|
| Default password | **Forced change on first use** | A fixed default is bad practice and likely non-compliant — California SB-327 and the UK PSTI Act each require a unique per-device credential or a forced change at setup. This board has no screen and no per-unit labelling step, so per-device randomness can't be communicated. |
| TLS | **No** | No CA issues certificates for a private IP, and a self-signed cert trains users to click through browser warnings — worse than no TLS, because it erodes the one signal that matters elsewhere. TLS also costs RAM on a device already running NAT and the relay. WPA2 on the SoftAP is the transport protection. |
| Password on the wire | **Challenge-response** — server issues a nonce, client returns `HMAC(stored_key, nonce)` | With WPA2-PSK, anyone who knows the AP passphrase can decrypt other clients' traffic. A team may share the Wi-Fi passphrase without every member being an administrator. |
| Browser crypto | **A bundled ~2 KB SHA-256/HMAC** | ⚠️ `crypto.subtle` is only exposed in *secure contexts*, and `http://172.16.50.1` is not one (only `localhost` is trusted over plain HTTP). **Do not "simplify" this back to WebCrypto later — it will silently be `undefined` on the device.** |
| Storage | PBKDF2-HMAC-SHA256, per-device random salt in NVS | mbedtls is already linked. Never store the password itself. Logins are rare, so err high on iterations. |
| Sessions | `esp_random()` tokens, RAM only, small fixed table, idle timeout, `HttpOnly` + `SameSite=Strict` | Tokens should not survive a reboot. |
| Brute force | Lockout or backoff | The attacker here is already on the LAN. |
| Recovery | `gwcfg-reset-auth` on the serial console, plus the BOOT-button reset | Physically-present-only is the right trust model. **An undocumented recovery path is the same as none** — document it prominently. |

### Networking

- **NAT for general traffic, an explicit relay for CoT multicast.** batman-adv is a Layer 2 mesh
  with no concept of routing to an IP subnet that only exists behind a NAT'd leaf, so a routed
  (non-NAT) design would need either a manual static route on the gateway Pi or a nonexistent
  dynamic mechanism for leaves to announce a subnet. NAT ships now and works; the cost is that
  **nobody on the mesh can originate a connection to a specific phone behind the XIAO**. Multicast
  is handled separately by the relay precisely because it doesn't depend on that unicast path.
  Static routing on the gateway Pi is the v2 fix if it's ever needed — not a firmware change.
- **The XIAO cannot L2-bridge its two radios** the way batman-adv bridges Pi nodes: they're
  physically different PHYs and there's no batman-adv on FreeRTOS/lwIP. L3 forwarding between the
  two `esp_netif`s is the only option, which is why the relay has to exist at all.
- **Default SoftAP subnet is 172.16.50.0/24.** 192.168.x collides with home routers, phone hotspots
  and esp_netif's own 192.168.4.1 default; an overlap between a client's remembered network and
  this one is very hard to diagnose in the field. Every node can safely use the same subnet — each
  NATs behind its own uplink address.
- **US-only, 902–928 MHz — and that is a hardware limit.** The module is a Quectel FGH100M-H, a
  902–928 MHz part, and the BCF the firmware loads (`bcf_fgh100mhaamd.bin`) is named "FGH100M-H
  (US)" upstream; it carries real calibration for US alone. The nine-region build matrix that used
  to live in `country-configs/` was checking `mmregdb`'s channel tables and nothing else — four of
  those regions were unreachable frequencies and two had no BCF section at all, all failing
  silently. Deleted. `design/HARDWARE.md` "Regulatory domain" has the section-by-section evidence.
  Don't re-add a region without a BCF that covers it.
- **Region is build-time and cannot be made runtime-configurable.** The SDK reads
  `CONFIG_HALOW_COUNTRY_CODE` from Kconfig before the radio scans; there is no per-connection
  channel argument in the STA connect API. It is now fixed at `"US"` in `sdkconfig.defaults`.
- **`CONFIG_HALOW_PS_MODE=n`, and it is not in the vendor's board file.** The component defaults it
  *on* whenever `CONFIG_MM_WAKE`/`CONFIG_MM_BUSY` are set — which upstream's own Seeed board config
  does — but its help text requires those pins to be physically connected, and on the HAT
  (rev V3.0) R10 and R17 are the board's only DNP resistors. Running power-save against pins that
  reach nothing lets the host stop servicing the SPI interrupt while the module is still awake;
  the resulting bus errors escalate to an `MMOSAL_ASSERT`, which in this SDK calls `esp_restart()`
  rather than returning an error, so the node boot-loops shortly after the connect attempt starts.
  **This looks exactly like a radio failure and is not one** — the radio initializes fine first.
  Do not remove the setting to "match upstream's board file"; the citation chain is in
  `sdkconfig.defaults` and [`HARDWARE.md`](HARDWARE.md). The correct way to get power-save back is
  to populate R10/R17.

- **A node with no uplink configured does not start the reconnect loop.** "Nobody has told this
  node which AP to join" and "the AP we were told about isn't answering" are different situations
  with opposite advice, and the firmware used to report both as `searching` while burning 15-second
  association attempts against a placeholder SSID it shipped with. Now the defaults carry *no*
  uplink, an empty SSID means "not configured" (`gw_uplink_is_configured()`), and that state has
  its own LED pattern, status string and web-UI banner. It also keeps the radio free for scanning,
  which is what an operator is doing at that exact moment. Derived from the SSID rather than a
  separate flag or a user-facing toggle: the two can never disagree, and there is no switch that
  can strand a configured node in the field.

### Observability

- **Flash core dumps are on** (64 KB partition, ELF format). Bring-up means crashes happen
  unattended and away from a terminal; without this a panic leaves nothing behind. Flash cost is
  trivial against 8 MB.
- **The reset reason is logged at every boot, into the RAM log ring.** Hardware bring-up produced a
  reboot loop on a node that could not be powered from a PC's USB port — so there was no serial
  console, and every instrument that survives a reset lives in flash or on the far side of a cable.
  `esp_reset_reason()` costs one line and separates a brownout from a panic from a watchdog, which
  is the first fork in that diagnosis. The core-dump summary is logged next to it but labelled
  separately: it is the last panic *ever* recorded, not necessarily this boot's.
- **Link state is four states, not a boolean.** "Not associated" and "associated but no lease" are
  different subsystems failing, with different fixes. Collapsing them was the single biggest
  diagnosability gap the firmware had.

## Known limitations — decisions, not bugs

Recorded so they don't get "fixed" by accident.

- **No inbound unicast to a specific client.** See the NAT reasoning above.
- **NAT and the relay initialize against the first uplink IP only.** If a later reconnect leases a
  *different* address they are not re-initialized against it. Whether this matters depends on
  whether lease changes happen in practice, which hardware testing will answer. (A *failed* init
  does retry on the next reconnect — that was a bug and is fixed. Different thing.)
- **No captive-portal DNS redirect.** A real UX gap, not a defect. See item 4 above.
- **No web UI authentication.** See item 1. The SoftAP passphrase plus the subnet check (and the
  interim Host/Content-Type CSRF guards) is the current boundary.
- **The web UI cannot clear a stored passphrase or switch the SoftAP to open.** A blank password
  field means "keep current" - `GET /api/config` never echoes passphrases back, so an empty field
  can't be distinguished from "clear it", and "keep" is the safe reading. The console can do it:
  `gwcfg-set-softap <ssid> -`. The page says so next to the field.
- **No OTA delivery.** Layout is ready, mechanism is blocked on auth by choice, not effort.
- **No radio power save.** `CONFIG_HALOW_PS_MODE=n` is forced off because the Seeed HAT leaves the
  WAKE and BUSY links unpopulated — see the settled decision below. Current draw is higher than the
  hardware could achieve, and that is the accepted price of not boot-looping.
- **`GW_CONFIG_VERSION` bumps discard stored config.** Deliberate: a layout or default change that
  silently reinterpreted an existing blob would be worse. Reprovision after a bump.

## Open questions

All Pi-side, all tracked in [`PI_SIDE.md`](PI_SIDE.md): the AP's security mode, DHCP scope and
whether it offers DNS, the regulatory domain (**this one blocks association outright**), and
mesh-point + AP concurrency once a second Pi joins.
