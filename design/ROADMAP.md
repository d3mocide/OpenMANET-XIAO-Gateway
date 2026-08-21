# Roadmap

Where this project is, what's next, and the decisions that shouldn't be re-made. **Start here if
you're picking the project back up.**

- Companion docs: [`HARDWARE.md`](HARDWARE.md) (what to buy, how to build one, how to bring it up),
  [`PI_SIDE.md`](PI_SIDE.md) (the other end of the link)
- Architecture diagram and repo layout: [`../README.md`](../README.md)
- **Last updated:** 2026-08-17

Keep this file current: tick the checklist when a step passes, move an item out of "not built yet"
when it lands, and add to "settled decisions" rather than re-arguing one. Historical detail
belongs in git history, not here — this file describes the present and the plan.

## Status at a glance

**Target hardware: Seeed XIAO ESP32-S3 + Seeed XIAO WM6108, whose radio module is a Quectel
FGH100M-H — 902–928 MHz, US only.** One build, `CONFIG_HALOW_COUNTRY_CODE="US"`; see "Settled
decisions" and [`HARDWARE.md`](HARDWARE.md) "Regulatory domain".

`idf.py build` **passes end-to-end** against ESP-IDF v5.5.1 with the real `morsemicro/halow`
component: **zero errors, zero warnings**, binary **~1.74 MB (`0x1be2f0`)**, **42% free** in the
3 MB app slot on confirmed 8 MB flash. Verified by actually running the build, not by reading code.

That is 182,464 bytes (178 KB, 9.1%) smaller than the ~1.92 MB / 36% this sat at through the
GW_ROLE_RELAY work, from two changes measured together on one build: `-Os` instead of ESP-IDF's
default `-Og` (~145 KB, and it applies to esp-halow's vendored hostapd/wpa_supplicant fork as much
as to this project's own code) and gzipping the embedded web UI (33,694 bytes). The slot now has
more headroom than it did before AP mode landed.

**First hardware bring-up is under way.** Steps 1 and 4 have passed on a real XIAO ESP32-S3 +
WM6108: the MM6108 answers over SPI with its version banner, and the SoftAP leases addresses and
serves the web UI. A compiling build proves the code is internally consistent against the real
APIs; it does not prove the radio associates, DHCP completes, or NAT and the CoT relay pass
traffic. Everything remaining in the checklist below is hardware-only from here.

**Stability confirmed under real, concurrent load.** Running off a PC's USB cable (no Pi present)
with a phone associated to the SoftAP and the HaLow radio initialized and periodically scanning,
the node stays up indefinitely — no reboot loop, no crash. This was the failure mode the
`CONFIG_HALOW_PS_MODE` fix (see "Settled decisions") targeted, and it holds with the SoftAP,
DHCP server, web UI and HaLow radio all running at once, not just at idle. **Next phase is
bringing up the Pi and testing the HaLow uplink against it — checklist steps 0 through 3.**

The first thing hardware taught us wasn't in the firmware at all: the HaLow HAT's unpopulated
WAKE/BUSY links make the SDK's *default* power-save setting reboot the node in a loop, which
presents identically to a dead radio. See `CONFIG_HALOW_PS_MODE` under "Settled decisions".

**It also doesn't prove the code means what it says.** Three separate passes have now found bugs in
clean-compiling firmware that would each have failed silently on hardware and looked like a radio
problem:

- the CoT relay matched `IP_PKTINFO`'s `ipi_addr` (the packet's *destination*, always the multicast
  group) instead of `ipi_ifindex`, dropping 100% of traffic while logging success;
- NAPT was enabled on the uplink netif when ESP-IDF requires it on the SoftAP netif;
- SoftAP clients were handed no DNS server, so every hostname lookup failed while raw IP worked;
- the CoT relay passed `recvmsg()`'s return value straight to its forwarding `sendto()`. On a UDP
  socket lwIP returns the *datagram's* length there, not the bytes copied into the iovec
  (`if (datagram_len > buflen) msg_flags |= MSG_TRUNC; ... return (int)datagram_len` — esp-lwip
  `2.2.0-esp`, `src/api/sockets.c` L1411-1417), so any CoT event over 1500 bytes made the relay
  read past its `.bss` receive buffer and transmit whatever followed it onto the mesh. Now dropped
  on `MSG_TRUNC`, with a log line;
- `use_static_ip` was fully plumbed — console command, web UI field, validation, `gwcfg-status`,
  `/api/config` — and **never read by `uplink_halow.c`**. Since a relay's HaLow AP deliberately
  runs no DHCP server, a leaf configured exactly as documented could only associate, wait out two
  30-second lease timeouts, disconnect and loop forever. It now applies the address at bring-up,
  and `esp_netif_action_connected()` raises `IP_EVENT_STA_GOT_IP` for it on every link-up.

All five were found by checking this firmware's assumptions against upstream ESP-IDF/lwIP source,
not by re-reading this repo. Assume the same class of error exists elsewhere. See
[`../CLAUDE.md`](../CLAUDE.md) for the working rule this produced. Note what the last two have in
common with the first three: every one of them compiles clean, and every one presents on hardware
as a radio or link problem.

**A fourth was found only by running it** (2026-08-17, item 8): the datapath bring-up was correct
against every API it called, and still crashed — because of *where* it ran, not what it did. Called
from an uplink state callback, it executed on the `sys_evt` event-loop task and overflowed that
task's 2816-byte stack, panicking the node into a reboot loop. Reading the call site tells you
nothing here; the bug lives in the execution context the call site inherits. When adding work to any
callback, check which task will run it and what stack that task has.

## What's implemented

| Module | File | Status |
|---|---|---|
| Local SoftAP + DHCP | `main/downlink_softap.c` | 2.4 GHz AP + DHCP server for phones/tablets/ATAK devices. GW_ROLE_CLIENT only. |
| HaLow STA uplink | `main/uplink_halow.c` | STA association, reconnect/backoff, bounded DHCP wait with disconnect-and-retry. Exposes a four-state link state, RSSI, a blocking scan wrapper, and radio version readback. GW_ROLE_CLIENT only. |
| Wi-Fi STA uplink | `main/uplink_wifi.c` | GW_ROLE_RELAY only - native `esp_wifi` STA joining the Pi's own local AP directly (item 8 below). Same link-state/RSSI/callback shape as `uplink_halow.c`, event-driven reconnect against standard ESP-IDF STA events rather than a blocking task. |
| HaLow AP downlink | `main/downlink_halow_ap.c` | GW_ROLE_RELAY only - HaLow radio in AP mode (`CONFIG_HALOW_AP_MODE`) so other XIAOs can associate to this node instead of a Pi (item 8 below). Static IP, no DHCP server - see the file's own header comment for why. Also exposes the regulatory channel table so an operator can pick a legal (op_class, s1g_chan_num) pair. **Untested on real hardware** - Morse's own AP-mode API is marked alpha. |
| NAT / IP forwarding | `main/ip_forward_nat.c` | All three steps of ESP-IDF's NAT recipe: DNS propagation into the SoftAP's DHCP offers, uplink as default route, NAPT on the downlink. |
| CoT multicast relay | `main/cot_relay.c` | One socket joined to 239.2.3.1:6969 on both netifs, `IP_PKTINFO`/`recvmsg()` for arrival interface, loop prevention via `IP_MULTICAST_LOOP` off + own-source drop. |
| Provisioning | `main/provisioning.c` | NVS config blob (magic + version stamped, validated on load and save) plus `gwcfg-*` console commands over USB Serial/JTAG. `gwcfg-set-role` selects GW_ROLE_CLIENT/GW_ROLE_RELAY at runtime - one firmware image, no separate relay build. |
| Web config UI | `main/web_ui.c` / `.html` | `esp_http_server` + embedded HTML. `GET /api/status`, `GET`/`POST /api/config`, `GET /api/log`, `GET /api/tasks`, `POST /api/scan`, `POST /api/reboot`. Same NVS config as the console. SoftAP clients only; **no authentication yet**. |
| Status LED | `main/status_led.c` | On-board GPIO21 LED blinks the uplink link state. The only instrument needing neither cable nor phone. |
| Factory reset | `main/factory_reset.c` | 5 s BOOT-button hold restores defaults and reboots; LED acknowledges at 1.5 s. |
| Stack headroom | `main/task_stats.c` | Worst-case free stack per task via `uxTaskGetStackHighWaterMark()`, surfaced as `gwcfg-tasks` and `GET /api/tasks`. Turns "is this close to overflowing?" into a number - see "Stack budgets" below. |
| Log ring buffer | `main/log_buffer.c` | `esp_log_set_vprintf` tee into a 6 KB RAM ring, served at `/api/log`. Chains to the previous handler, so serial output is unaffected. |
| App wiring | `main/app_main.c` | Brings up log buffer, LED, factory-reset watcher, console and web UI immediately; then one of two role-specific bring-up paths (`bring_up_client_role()` / `bring_up_relay_role()`). NAT + CoT relay come up via a shared helper once whichever uplink holds a usable IP, retrying on the next reconnect if that fails. |
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
      hardware test. Since re-confirmed running for an extended period on USB power with a client
      associated and the HaLow radio concurrently up and scanning — no reboot loop, no crash.
      **Next up: steps 0–3, which need the Pi side present.**
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

### 7. XIAO as a real 802.11s mesh point — *v2 scope, not this gateway's, needs Morse Micro*

Raised 2026-08-16 as a fallback if `PI_SIDE.md` item 0 (getting the Pi to beacon a HaLow AP at all)
turns out to be a dead end. **This is not a small addition to the current design — it's a separate,
much larger undertaking**, and most of the hard part isn't ours to build. Two independent gaps stack
on top of each other:

**Gap 1 — 802.11s mesh association itself.** Read directly from Morse Micro's own source
(`github.com/MorseMicro/esp-halow`, `github.com/MorseMicro/mm-iot-sdk`, fetched 2026-08-16):
`esp-halow`'s `hostap` component (`halow/components/hostap/CMakeLists.txt`) vendors a real fork of
upstream `wpa_supplicant`/`hostapd` (file names match hostap.git exactly), and builds genuine STA
code plus genuine AP code gated behind `CONFIG_HALOW_AP_MODE` (`hostapd.c`, `beacon.c`,
`ap_mlme.c`, `ieee802_11_s1g.c`, `NEED_AP_MLME` — the real thing, not a stub; this is what backs
the AP-mode support `HARDWARE.md` already notes exists on ESP32-C5). **`CONFIG_MESH` is never
defined and none of `mesh.c`/`mesh_mpm.c`/`mesh_rsn.c` are in the build** — but those exact files
**do exist**, unused, in the vendored source tree
(`mm-iot-sdk/framework/src/hostap/wpa_supplicant/mesh*.c`). So the gap isn't "Morse never touched
mesh code" — it's "the mesh code was vendored along with everything else and never wired into the
ESP32 build or given a driver to run on."
  That driver is the real blocker. `drivers_morse.c` registers exactly two `wpa_driver_ops` tables
  — `mmwlan_wpas_ops` (STA) and `mmwlan_wpas_ops_ap` (AP) — both `extern`, both "implemented by
  morselib." **`morselib` is Morse Micro's closed-source static library**; nothing in the public
  repos shows what it actually implements. Whether a hypothetical `mmwlan_wpas_ops_mesh` is
  feasible depends on primitives (peer-specific keys, self-protected action frames, mesh beaconing)
  that only Morse Micro can see or add. **We cannot build this ourselves from outside their SDK.**
  The one strong reason to think it's tractable *for them*: the identical silicon (MM6108, and
  specifically this project's exact FGH100M-H part) already runs 802.11s mesh point mode today,
  unmodified, on the Pi side via Linux's `hostapd_s1g`/`wpa_supplicant_s1g` — so this is a host-SDK
  porting gap, not a chip/RF capability wall. That's the pitch worth taking to Morse Micro: they've
  already vendored the mesh source and already proved their AP-mode driver ops can do STA/peer
  management; finishing the mesh port is a bounded ask, not "invent mesh support."
  (Note: an earlier PI_SIDE.md line reading "ESP32 HaLow cannot do STA-to-STA direct links,
  confirmed by Morse's own team" predates this finding and its original context wasn't recovered
  from this repo's history. It may have meant exactly this — not supported *today* — rather than
  "architecturally impossible." Worth re-asking Morse directly with this level of specificity
  before assuming it's a closed door.)

**Gap 2 — batman-adv, or an equivalent, doesn't exist for FreeRTOS/lwIP at all.** Even a fully
working 802.11s mesh association only gets the XIAO to "one MAC-layer peer." OpenMANET's actual
mesh behaviour — the flat L2 domain, multi-hop routing, loop prevention — comes from batman-adv
running on top of the 802.11s interfaces (see "Settled decisions" below, already established). That
is Linux kernel networking code with no FreeRTOS/lwIP counterpart to build from; porting or
reimplementing it is new work with no existing scaffolding, and plausibly a bigger lift than gap 1.
Skipping it and just associating at 802.11s without batman-adv is not a safe substitute — a plain
mesh peer with no OGM exchange sitting next to batman-adv-speaking neighbors is undefined behaviour
this project has not tested, not a known-working "leaf mesh point" mode.

**Where this leaves the plan**: `PI_SIDE.md` item 0 (a Pi-side AP-mode config test) is orders of
magnitude cheaper than this and should be exhausted first — it needs no new code anywhere. This item
is the real fallback, and starts with a conversation with Morse Micro about gap 1, not firmware work
in this repo. Gap 2 needs a considered scoping pass of its own before any code gets written, even if
Morse Micro says yes to gap 1.

### 8. GW_ROLE_RELAY — built 2026-08-16, first hardware run 2026-08-17, **partially confirmed**

Fallback for the worst case on both item 0 (Pi has no HaLow AP mode) and item 7 (Morse Micro won't
add mesh support): route around HaLow entirely for the Pi-facing hop, using only things already
confirmed to exist - `esp-halow`'s real `CONFIG_HALOW_AP_MODE` support (see item 7), the ESP32-S3's
own native 2.4 GHz Wi-Fi (fully separate radio from the HaLow module), and OpenMANET's
already-documented, always-on local AP (`br-ahwlan`, bridged into `bat0`) that every mesh point
exposes with its own DHCP. **Needs zero changes on the Pi or from Morse Micro.**

Landed as a second node **role**, selectable at runtime (`gwcfg-set-role client|relay`, or the web
UI's Node → Role field) - one firmware image serves both, not a separate build:

- **GW_ROLE_CLIENT** is unchanged - today's original design, still the default.
- **GW_ROLE_RELAY** is new: `main/uplink_wifi.c` (native `esp_wifi` STA joining the Pi's local AP,
  event-driven reconnect against standard ESP-IDF STA events - the validation step this project
  needed anyway, since nothing past `gwcfg-scan` had run against a real OpenMANET mesh) plus
  `main/downlink_halow_ap.c` (HaLow radio in AP mode via `mmhalow_set_config(WIFI_IF_AP, ...)` +
  `mmhalow_wifi_start()`, static IP rather than DHCP - see that file's header comment for why
  `esp_netif`'s DHCP server can't be attached to the netif `mmhalow_init()` creates). A
  GW_ROLE_CLIENT leaf needs zero firmware changes to associate to a relay instead of a Pi - it's the
  same `uplink_halow.c`, just pointed at the relay's SSID, with an optional static-IP override
  (`gwcfg-set-uplink-static-ip`) since the relay's HaLow AP doesn't hand out leases. One relay can
  serve multiple leaf XIAOs (HaLow AP mode is multi-client like any AP), and a field-deployed leaf
  keeps HaLow's actual long range to reach the relay - only the relay-to-Pi hop is short-range Wi-Fi,
  which only has to cover "near the Pi."

`gwcfg-set-wifi-uplink` and `gwcfg-set-halow-ap` (plus `gwcfg-list-halow-channels`, since
`mmwlan_ap_args` wants an (op_class, s1g_chan_num) pair with no obvious mapping to a frequency an
operator actually has) configure the two relay-only radios; `gwcfg-status`, `/api/config` and
`/api/status` all became role-aware. `GW_CONFIG_VERSION` bumped to 4 for the new fields - reprovision
after updating, per the usual policy.

**Confirmed by build, not yet by hardware**: `idf.py build` passes clean against ESP-IDF v5.5.1 with
`CONFIG_HALOW_AP_MODE=y` now always on (binary grew from ~1.67 MB/44% free to ~1.89 MB/37% free -
built into every image since role is a runtime choice, not a build-time one). Both figures are
pre-`-Os`; the slot is back to 42% free since - see "Status at a glance". What building does
*not* prove: Morse Micro's own header marks `mmwlan_ap_args`/`mmwlan_ap_enable()` "ALPHA NOTICE:
under development; breaking changes may be introduced," and while ESP32-S3+MM6108 is in
`esp-halow`'s tested-hardware table, its README doesn't break testing out by mode - AP mode
specifically on this exact chip pairing isn't proven on real hardware the way STA mode already is.
Budget bring-up time for GW_ROLE_RELAY like any other untested path in this project (see
`design/HARDWARE.md`'s runbook pattern) before trusting it in the field. First things to check on
real hardware. Procedure and pass/fail criteria for each is in
[`HARDWARE.md`](HARDWARE.md) Part 3 - same relationship as the main checklist above has to
`HARDWARE.md` Part 2: this is the tracker, that is the runbook.

- [ ] **Tier 0** - does `mmhalow_wifi_start()` actually bring the HaLow AP up on air at all (it
      returns `void`, so `downlink_halow_ap_is_started()` only means "we called it," not
      confirmation)? Two XIAOs, no Pi, no Wi-Fi network needed - the cheapest, most isolated check
      of the alpha AP-mode API by itself.
- [ ] **Tier 1** - does a leaf's `gwcfg-set-uplink` against that AP actually associate, and does the
      static-IP-on-both-ends addressing scheme (no DHCP server on a relay's HaLow AP - see
      `main/downlink_halow_ap.h`) actually pass traffic once associated? **Note this could not have
      passed before**: `use_static_ip` was stored and validated but never applied to the netif, so
      the leaf ran a DHCP client against an AP with no DHCP server. Fixed — `apply_static_ip()` in
      `main/uplink_halow.c`, which is also where the derivation lives. Untested on hardware, so
      treat a Tier 1 failure as a real result rather than assuming this fix settled it.
- [ ] **Tier 2** - the full chain against *any* ordinary Wi-Fi network on the relay's uplink side
      (still no Pi needed) - first real-hardware run of the NAT/DNS/CoT-relay pipeline at all.
      **Attempted 2026-08-17 against an ordinary WPA3-SAE home AP.** The uplink half passed and the
      datapath half found a bug; both are recorded under "What the first relay run proved" below.
- [ ] **Tier 3** - the actual target scenario: swap "any Wi-Fi network" for the Pi's own local AP.
      Only meaningful once item 0's AP-mode workaround is confirmed on the real Pi.

#### What the first relay run proved (2026-08-17)

A relay node configured for a 4 MHz HaLow AP (`op_class 3`, `s1g_chan_num 8`, SAE) with a WPA3-SAE
home AP as its Wi-Fi uplink. Confirmed on hardware, from that boot's serial log:

- **The two-radio netif split works.** `uplink_wifi.c` creates its STA netif under the custom if_key
  `WIFI_STA_NATIVE` precisely because `mmhalow_init()` has already claimed `WIFI_STA_DEF` for the
  HaLow radio. Both netifs coexisted with no duplicate-key panic in `esp_netif_new_api()` — the
  failure that comment was written to prevent. `esp_netif_create_wifi()` +
  `esp_wifi_set_default_wifi_sta_handlers()` is a working substitute for
  `esp_netif_create_default_wifi_sta()`.
- **The Wi-Fi uplink works end-to-end.** Associated to WPA3-SAE at -53 dBm, then took a DHCP lease
  (`sta_native ip: 192.168.50.128`) and raised its state callback. `uplink_wifi.c` is no longer an
  unproven path.
- **`downlink_halow_ap_init()` runs clean** through `mmhalow_wifi_start()` with no error, on top of
  a working SPI link to the MM6108 (version banner printed). This is *not* Tier 0: that call
  returns `void`, so this still only means "we called it," not that the AP is on air.

It also found a real bug, now fixed: **the datapath bring-up overflowed the event loop task's
stack** the moment the uplink got its lease, panicking the node into a reboot loop.
`bring_up_datapath()` (NAT + DNS + CoT relay) was being called straight from the uplink state
callback, which both uplink modules raise from inside an `esp_event` handler — so it ran on
`sys_evt`, whose stack is 2816 bytes total in this build and nowhere near enough. It now runs on its
own 4096-byte `datapath` task and the callback only posts a notification. The full derivation, with
upstream citations, is on `datapath_task()` in `main/app_main.c`.

Two things worth carrying forward from that:

- **This was latent in GW_ROLE_CLIENT too**, not a relay-only bug — that role calls the same
  `bring_up_datapath()` from the same kind of handler in `uplink_halow.c`. It surfaced on the relay
  first only because the relay is what got run. Any new uplink state callback must stay short; the
  typedefs in `uplink_halow.h`/`uplink_wifi.h` now say so.
- **The relay's downlink has no DHCP server, and `ip_forward_nat.c` now checks before assuming
  one.** `mmhalow_init()` builds its netif from `ESP_NETIF_DEFAULT_WIFI_STA()`, so `esp_netif`
  never allocates a `dhcps` handle for it and every DHCP-server call against it fails by
  construction. That is correct — leaf nodes on this hop are statically addressed by design — but
  it was producing two `ESP_LOGE` lines per relay boot about a server that was never meant to
  exist. DNS propagation is now skipped, with a log line saying why, when the downlink netif lacks
  `ESP_NETIF_DHCP_SERVER`.

Both the client and relay roles keep every known HaLow constraint from `PI_SIDE.md`: security must
be `open`/`owe`/`sae` (no PSK), region is fixed `US`, and phones behind any leaf XIAO stay NAT'd
(`172.16.50.x`, not
`10.41.x.x`) exactly as today — L2 bridging is still off the table on ESP32/lwIP regardless of which
radio carries the uplink.

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

### Stack budgets

After the `sys_evt` overflow (item 8), every task and callback context in the firmware was audited.
Recorded so the numbers don't have to be re-derived, and so the non-obvious ones aren't "tidied".

**These are the sizes tasks are created with, not the headroom they actually have.** For that, run
`gwcfg-tasks` on the console or open `GET /api/tasks` (Live Log tab → Task Stack Headroom) — both
report `uxTaskGetStackHighWaterMark()`, the least stack each task has ever had spare. Prefer the
measurement to this table when deciding whether a budget is too tight: the table says what was
allocated, the instrument says what got used. Check it after the node has been up a while and
through a reconnect or two, since a high-water mark only reflects paths that have actually run.

| Context | Stack | Notes |
|---|---|---|
| `sys_evt` (esp_event default loop) | **2816** | 2304 Kconfig default + 512 `TASK_EXTRA_STACK_SIZE`. Not overridden. **The tightest budget in the system, and it runs every event handler.** |
| `esp_timer` task | 4096 | 3584 + 512. Runs `reconnect_timer_cb`, `reboot_timer_cb` — both trivial by design. |
| `datapath` | 4096 | Where NAT + CoT relay bring-up actually runs. **Exits once the datapath is up**, returning the 4 KB — so `absent` is the healthy steady state in `gwcfg-tasks`, and still-present means an attempt is outstanding. |
| `wifi_reconnect`, `halow_reconnect`, `cot_relay`, `factory_reset` | 4096 each | |
| httpd (`web_ui.c`) | 6144 | Raised from esp_http_server's 4096 default. |
| console REPL (`provisioning.c`) | 4096 | `ESP_CONSOLE_REPL_CONFIG_DEFAULT()`. |
| `status_led` | 2048 | Deliberately small — the task body reads an enum and toggles a pin. It must stay that way; it has no room for a log call. |
| morselib `evtloop` (SDK-owned) | 8608 | `umac_evtloop.c` asks for 2152 **words**; the ESP32 shim converts (`stack_size_u32 * 4`, `mmosal_shim_freertos_esp32.c` L216-221). This is what invokes `mm_sta_state_cb` and `scan_rx_cb` — so `web_ui.c`'s cJSON scan collector runs on an SDK task, not ours, and fits. |

Two things that look like ordinary style but are load-bearing:

- **`cot_relay.c`'s 1500-byte `rx_buffer` is `static`.** As a local it would be over a third of that
  task's stack in one object.
- **`log_buffer.c`'s `s_line[256]` is `static`, guarded by the ring's own mutex.** That hook is
  installed via `esp_log_set_vprintf`, so it runs on *whatever task called `ESP_LOGx`* — as a local
  it charged 256 bytes to every task in the firmware on every log call, `sys_evt` included, on top
  of the console handler's `vprintf`. Moving it to `.bss` costs nothing in flash and removes ~9% of
  `sys_evt`'s stack from every logged line.

Anything added to an event handler, an `esp_timer` callback, or a callback the HaLow SDK invokes
inherits one of these budgets rather than getting its own. Check which one before adding work.

## Known limitations — decisions, not bugs

Recorded so they don't get "fixed" by accident.

- **No inbound unicast to a specific client.** See the NAT reasoning above.
- **NAT and the relay initialize against the first uplink IP only.** If a later reconnect leases a
  *different* address they are not re-initialized against it. Whether this matters depends on
  whether lease changes happen in practice, which hardware testing will answer. (A *failed* init
  does retry on the next reconnect — that was a bug and is fixed. Different thing.) Because of
  this, the `datapath` task now exits once both halves are up and returns its 4 KB stack; it stays
  alive only while an attempt is still outstanding. If this limitation is ever lifted, that exit
  has to go with it.
- **A statically-addressed uplink gets no DNS.** `gwcfg-set-uplink-static-ip` means no DHCP
  client, so nothing learns a resolver — and `esp_netif_set_ip_info()` additionally calls
  `dns_clear_servers(true)` on a DHCP-client netif. `ip_forward_nat.c` handles it correctly
  (warns, and leaves the downlink's DHCP DNS option off rather than offering `0.0.0.0`), so leaf
  clients get working IP connectivity and no name resolution. Acceptable on a hop whose purpose is
  CoT, which is addressed by IP. A configurable static DNS server is the fix if it ever matters.
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

All Pi-side, all tracked in [`PI_SIDE.md`](PI_SIDE.md). **Top of the list as of 2026-08-16:**
whether any current OpenMANET role still exposes a HaLow radio in plain AP mode at all — reading
OpenMANET's own docs, the "HaLow AP wizard" appears to have been removed, and every HaLow radio is
now described identically as a `10.41.0.0/16` backbone member. If that holds, it blocks association
**more fundamentally** than the regulatory domain does, on any Pi, not just once a second one joins
— see `PI_SIDE.md` "Still to verify" item 0 for the citations and what to check on a real Pi before
trusting the rest of this list. Below that: the AP's security mode, DHCP scope and whether it offers
DNS, the regulatory domain (**this one blocks association outright**), and mesh-point + AP
concurrency once a second Pi joins.
