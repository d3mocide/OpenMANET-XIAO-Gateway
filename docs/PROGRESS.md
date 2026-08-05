# Progress / Roadmap

Status tracker for the XIAO HaLow gateway firmware. Read this first if you're picking the
project back up (human or agent) - it says what's real, what's stubbed, and what to do next.
Read [`DESIGN.md`](DESIGN.md) for the full design and [`pi_side_reference.md`](pi_side_reference.md)
for Pi-side facts before making changes. Update this file's checklist whenever you finish a step
below or learn something that changes it.

- **Branch:** `claude/xaio-client-node-design-lkv9og`
- **PR:** https://github.com/d3mocide/OpenMANET-S3-Client/pull/1
- **Last updated:** 2026-08-05

## Status at a glance

The ESP-IDF project skeleton from `DESIGN.md` §7 is scaffolded and structurally wired end to
end. It has **not been compiled or run** - the environment this was authored in had no ESP-IDF
toolchain installed, so nothing here has been verified with `idf.py build`, let alone on real
hardware. Treat everything below as "should be correct, not yet proven."

One piece is a deliberate, clearly-marked stub rather than best-effort guesswork: the actual
HaLow radio bring-up. See "The one blocking gap" below.

## What's implemented

| Module | File | Status |
|---|---|---|
| Local SoftAP + DHCP | `main/downlink_softap.c` | Implemented against stock `esp_wifi`/`esp_netif` APIs. Not yet built/tested. |
| NAPT (uplink NAT) | `main/ip_forward_nat.c` | Implemented via `esp_netif_napt_enable()`, called once the uplink first reports connected. Not yet built/tested. |
| CoT multicast relay | `main/cot_relay.c` | Implemented: single socket joined to 239.2.3.1:6969 on both netifs, uses `IP_PKTINFO`/`recvmsg()` to identify arrival interface and avoid a forwarding loop. Not yet built/tested. |
| Provisioning (NVS + console) | `main/provisioning.c` | Implemented: `gwcfg-show` / `gwcfg-set-uplink` / `gwcfg-set-softap` / `gwcfg-set-node` / `gwcfg-save` / `gwcfg-reset` over the serial console, NVS blob load/save, placeholder defaults. Not yet built/tested. |
| App wiring | `main/app_main.c` | Brings up SoftAP + console immediately; brings up NAT + CoT relay on first uplink connect. Not yet built/tested. |
| HaLow STA uplink | `main/uplink_halow.c` | **Stubbed.** Reconnect/backoff state machine is real; the three functions that would actually talk to the `morsemicro/halow` component (`halow_sta_bringup`, `halow_sta_connect`, `halow_sta_link_up`) just log and return failure/false. See below. |

## The one blocking gap: HaLow radio integration

`main/uplink_halow.c` has a large comment block (top of the file) explaining why this is stubbed:
no ESP-IDF toolchain was available in the authoring environment to check the real
`morsemicro/halow` component headers, and shipping guessed function names as if verified would
be worse than an honest stub - it could silently fail to compile once the component is actually
fetched, or worse, compile against the wrong assumptions and misbehave.

**To unblock:** on a machine with ESP-IDF + the component installed:

1. `idf.py build` once (pulls `morsemicro/halow` into `managed_components/` per
   `main/idf_component.yml`).
2. Read `managed_components/morsemicro__halow/include/*.h` for the real init/config/connect API.
3. Fill in the three stub functions in `main/uplink_halow.c`:
   - `halow_sta_bringup()` - one-time radio + netif creation from `gw_uplink_config_t`
   - `halow_sta_connect()` - associate + get an IP (blocking with timeout is assumed by the
     caller; adjust the reconnect loop in the same file if the real API is event-driven instead)
   - `halow_sta_link_up()` - poll/query current link state
4. Everything else (NAT, CoT relay, reconnect/backoff, state callback into `app_main.c`) should
   need no changes - they were written against these three functions' contracts specifically so
   the rest of the pipeline wouldn't need to change once this lands.

## Build-order checklist (DESIGN.md §8)

- [ ] **Step 0** - Confirm the Pi's HaLow radio config (`hostapd_s1g` AP mode, SSID/security/
      channel). See `pi_side_reference.md` open items 1 and 3. Nothing in firmware is blocked on
      this (config is provisioned via `gwcfg-set-uplink`, not hardcoded), but real values are
      needed before step 1 can succeed.
- [ ] **Step 1** - HaLow STA association works against that config. Blocked on "the one blocking
      gap" above.
- [ ] **Step 2** - DHCP lease + reachability confirmed (ping a Pi node; confirm the lease is
      visible from the Pi side). Blocked on step 1.
- [ ] **Step 3** - Local SoftAP + DHCP validated standalone (phones can join, get a lease).
      Code is written and should be testable **today**, independent of steps 0-2 - this is the
      natural first thing to verify once there's hardware in hand.
- [ ] **Step 4** - NAPT validated (phone gets outbound mesh/internet reach). Code is written
      (`ip_forward_nat_init`); needs a working uplink (step 1-2) to test for real.
- [ ] **Step 5** - CoT multicast relay validated (ATAK on a phone sees mesh CoT and vice versa).
      Code is written (`cot_relay_start`); needs a working uplink to test for real.
- [ ] **Step 6** - Provisioning/config UX pass. Serial console (`gwcfg-*`) exists; no onboard
      config portal (e.g. captive portal / BLE) has been built - console-only is the current
      state, which DESIGN.md §5.6 calls a valid "standard ESP-IDF pattern" option.

## Open questions

Tracked in detail in [`pi_side_reference.md`](pi_side_reference.md) - none of them block
firmware work today since the relevant values are provisioned, not hardcoded, but step 0/1 above
can't be *validated* without them:

1. Pi's HaLow AP security mode (open/PSK/SAE)
2. DHCP scope/lease ownership (the associated Pi vs. centralized by `openmanetd`)
3. Regulatory/channel plan the Pi's HaLow radio uses
4. Mesh-point + AP concurrency on one radio, once a second Pi joins (doesn't block a
   single-Pi build)

## Known v1 limitations (intentional, not bugs)

- `app_main.c`'s `on_uplink_state()` only initializes NAT/CoT relay on the **first** uplink
  connect. If a later reconnect gets a different DHCP-leased IP, they are not re-initialized
  against it. Documented in a comment there; fine for v1, worth revisiting if lease changes turn
  out to happen in practice.
- No inbound-unicast-to-a-specific-phone routing (NAT-only for v1, per `DESIGN.md` §4.3). Static
  routing on the gateway Pi is the suggested v2 fix if needed.
- No self-beacon (GPS/battery/status CoT) yet - `cot_relay_inject()` exists as the generic
  send primitive `DESIGN.md` §5.5 asks for, but nothing calls it yet.
