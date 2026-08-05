# Progress / Roadmap

Status tracker for the XIAO HaLow gateway firmware. Read this first if you're picking the
project back up (human or agent) - it says what's real, what's verified, and what to do next.
Read [`DESIGN.md`](DESIGN.md) for the full design, [`pi_side_reference.md`](pi_side_reference.md)
for Pi-side facts, and [`TECHNICAL_REVIEW.md`](TECHNICAL_REVIEW.md) for the pre-hardware review
findings before making changes. Update this file's checklist whenever you finish a step below or
learn something that changes it.

- **Branch:** `claude/hardware-testing-readiness-8s2tde`
- **Last updated:** 2026-08-05

## Hardware-testing readiness pass (2026-08-05)

The most recent pass. Its purpose was narrow: make the *first contact with real hardware*
diagnosable, and fix what a review found before it costs bench time.

**One confirmed defect, same class as the two the earlier review caught** - compiles clean, logs
success, fails on hardware in a way that points at the wrong subsystem:

- **SoftAP clients were handed no DNS server.** `ip_forward_nat_init()` did two of the three things
  ESP-IDF's NAT recipe requires (default route, NAPT) and never propagated the uplink's DNS server
  into the SoftAP's DHCP server. Verified against pinned v5.5.1 source, not from memory:
  `components/lwip/apps/dhcpserver/dhcpserver.c:172` initializes `dhcps->dhcps_dns = 0x00` and
  line 466 only emits the option when explicitly enabled, while
  `examples/wifi/softap_sta/main/softap_sta.c:168-177` is the reference implementation of the step
  we were missing. **Failure mode:** phones associate, get an address and a gateway, and every
  hostname lookup fails while raw IP works - which during step 4 looks exactly like "NAT is
  broken". Android additionally flags the network as having no internet and may fall back to
  cellular. Fixed.

**Two robustness bugs that would have made the first flash hard to read:**

- `app_main()` started the reconnect task even when `uplink_halow_init()` had failed. Against an
  uninitialized radio - the single most likely first-hardware failure - that spins forever, and its
  once-per-second error log buries the one line saying what actually went wrong. It now refuses to
  start, says so clearly, and leaves SoftAP/web UI/console up so the node is still workable.
- `on_uplink_state()` set its "already done" flag *before* attempting NAT and the CoT relay, so a
  single transient failure permanently disabled the datapath until a power cycle. The flag is now
  set only on success, and a failed attempt retries on the next reconnect.

**Bring-up instrumentation, which was the real gap** - the firmware could fail in five distinct
ways that all presented identically:

| Added | Answers |
|---|---|
| `uplink_link_state_t` + `/api/status.uplink.state` | "not associated" vs. "associated, no lease" - DESIGN.md §8 steps 1 and 2, with completely different causes. A single boolean collapsed them. |
| `gwcfg-radio` / auto-logged at boot | Does host↔MM6108 SPI work? Rules out wiring/pins/BCF/chip in one command. |
| `gwcfg-scan` + web UI scan button | Is the Pi's AP audible, on what channel, at what strength? Results are clickable to fill the SSID field. Prints the scanned region, so "AP is down" can be told apart from "AP is on a channel this build may not use". |
| RSSI (`mmwlan_get_rssi()`) on console + status panel | Separates "configured wrong" from "configured right but too weak". |
| Status LED (GPIO21) | Link state with no cable and no phone attached. |
| `log_buffer.c` + `/api/log` | The last ~6KB of log, over HTTP. Coredumps cover crashes; this covers "it's running and misbehaving". |
| BOOT-button factory reset | Config recovery without a cable. |

**Also in this pass:**

- Default SoftAP subnet **192.168.50.0/24 → 172.16.50.0/24** (`GW_CONFIG_VERSION` bumped to 2).
  192.168.x collides with home routers, phone hotspots and esp_netif's own 192.168.4.1 default; an
  overlap between a client's remembered network and this one is very hard to diagnose in the field.
- `CONFIG_HALOW_COUNTRY_CODE` default `"??"` → `"US"`, so a from-source build produces a firmware
  whose radio can actually come up. A fallback, not a deployment claim - the web flasher's region
  picker remains where the real choice is made.
- **CI now builds on pull requests** (US only - the regions differ by one Kconfig string, so nine
  builds find nine copies of the same error). Deploy jobs are gated to non-PR events.
- New docs: [`HARDWARE.md`](HARDWARE.md) (BOM, board pairing, pin map, antennas, power),
  [`BRINGUP.md`](BRINGUP.md) (the runbook), [`FEATURES.md`](FEATURES.md) (what isn't built, what
  each involves, in what order).

**A documented open question turned out to have an answer.** Earlier notes said no
`mmhalow_disconnect()` could be confirmed against the real component headers, and the DHCP-failure
recovery path worked around its absence by re-calling `mmhalow_connect()` on a live association.
The component's public `mmhalow.h` does declare `esp_err_t mmhalow_disconnect()` (implemented as
`mmwlan_sta_disable()`), confirmed by fetching v2.11.2-esp32-2 from the ESP Component Registry and
reading it. That path now disconnects properly first.

**Build re-verified after all of the above:** `idf.py build` against ESP-IDF v5.5.1, **zero errors,
zero warnings**, binary `0x1AB1C0` (1.67MB), **44% free** in the 3MB app slot, and `flash_args`
still confirms `0x10000 ota_data_initial.bin` / `0x20000 xiao_halow_gateway.bin` - the offsets the
web flasher manifest asserts.

**Still not flashed to hardware.** Everything above makes the first flash *legible*; none of it
makes it *proven*.

## Status at a glance

`idf.py build` **passes end-to-end** against ESP-IDF v5.5.1 with the real `morsemicro/halow`
component fetched from the ESP Component Registry - verified by actually running the build in a
throwaway ESP-IDF checkout, not just by reading code. All of `main/*.c`, including the HaLow STA
uplink and the on-device web config UI, compiles clean against the real component headers.

**Re-verified after the technical-review fixes (2026-08-05):** clean build, **zero errors and zero
warnings**, binary `0x1A4620` (1.64MB) with 45% free in the 3MB app partition on confirmed real
8MB flash. Every ESP-IDF/lwIP API the fixes rely on was checked against the pinned v5.5.1 source
in that same checkout rather than from memory.

**Re-verified again after the dual-OTA partition switch (2026-08-05):** `idf.py fullclean` +
clean rebuild, zero errors, zero warnings. Generated partition table matches `partitions.csv`
exactly, `ota_data_initial.bin` is produced, and the build's own `flash_args` confirms
`0x10000 ota_data_initial.bin` / `0x20000 xiao_halow_gateway.bin` - the offsets the web flasher
manifest now encodes and asserts.

**What that build pass does *not* mean:** nothing has been flashed to real hardware. A compiling
build proves the code is internally consistent against the real APIs; it doesn't prove the HaLow
radio actually associates, DHCP actually completes, or NAT/CoT relay actually pass traffic. See
"Build-order checklist" below for what's still real-hardware-only.

**It also doesn't prove the code means what it says.** A pre-hardware review
([`TECHNICAL_REVIEW.md`](TECHNICAL_REVIEW.md)) found two bugs in this clean-compiling firmware
that would each have failed silently on hardware and looked like radio problems: the CoT relay
dropped 100% of traffic (matched `IP_PKTINFO`'s `ipi_addr`, which carries the packet's
*destination* - always the multicast group - instead of `ipi_ifindex`), and NAPT was enabled on
the uplink netif when ESP-IDF requires it on the SoftAP netif. Both are fixed. Both were caught by
checking upstream ESP-IDF/lwIP source, not by re-reading this repo. Assume the same class of error
exists elsewhere and verify API semantics against upstream rather than inferring them.

## How this was verified (so it can be redone)

No CI is wired up yet, so this was done by hand in a scratch environment:

```sh
git clone -b v5.5.1 --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf && git submodule update --init --recursive --depth 1
./install.sh esp32s3   # needs libusb-1.0-0 (apt) for openocd-esp32, else it
                        # silently aborts before finishing the Python venv setup
source export.sh
cd /path/to/OpenMANET-XIAO-Gateway
idf.py set-target esp32s3
idf.py build
```

Worth repeating after any change to `main/*.c`, `sdkconfig.defaults`, or `main/idf_component.yml`
- it's the only way to catch a real compile/config error before it costs hardware-debugging time.
**Pitfall:** if you pipe `idf.py build` through `tail` (e.g. to keep output short), the reported
shell exit code reflects `tail`, not the build - it silently claims success on real failures. Grep
the actual log for `ERROR`/`FAILED` instead of trusting `$?` through a pipe.

## What this build pass found and fixed

None of these were guessed - each was a real error or a fact pulled directly from
`managed_components/morsemicro__halow` source after the component was actually fetched:

1. **Console over the wrong peripheral.** `sdkconfig.defaults` had
   `CONFIG_ESP_CONSOLE_UART_DEFAULT`, but the XIAO ESP32-S3's USB-C port is wired to the native
   USB Serial/JTAG controller, not an external UART bridge - logs and the `gwcfg-*` console would
   have been silent over the flashing cable. Fixed: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`, and
   `provisioning_start_console()` now branches on the sdkconfig console choice (mirrors ESP-IDF's
   own console example) instead of hardcoding the UART REPL.
2. **`idf_component.yml`'s IDF version floor was wrong.** A real dependency solve against ESP-IDF
   v5.3.1 failed: `morsemicro/halow@2.11.2-esp32-2` requires `idf >=5.4.4,<6.0`. The component
   name/namespace/version pin itself was correct on the first try. Fixed the constraint; the repo
   now targets v5.5.1 in local testing (anything in the 5.4.4-5.x range should work).
3. **No WPA2-PSK on HaLow.** `enum mmwlan_security_type` in the real SDK's `mmwlan.h` only has
   `MMWLAN_OPEN` / `MMWLAN_OWE` / `MMWLAN_SAE`. `gw_config.h`'s `gw_security_mode_t` and
   `gwcfg-set-uplink` were wrong to offer `psk`; fixed to `open|owe|sae`. `DESIGN.md` §4.1 and
   `pi_side_reference.md` corrected to match.
4. **No per-connection channel in the real API.** Dropped the `channel` field from
   `gw_uplink_config_t` - channel comes from the regulatory domain
   (`CONFIG_HALOW_COUNTRY_CODE`), a *build-time* Kconfig value, not something `gwcfg-*` can set.
   Currently the component's own placeholder default (`"??"`) - **must be set to a real country
   before flashing real hardware**, see `pi_side_reference.md` item 3.
5. **The component wouldn't even configure without board-specific Kconfig.** A real
   `idf.py build` failed at CMake time: `CONFIG_MM_CHIP`/`CONFIG_MM_BCF_FILE` and five SPI/GPIO
   pin configs are required. The component ships an exact match for this hardware pairing at
   `managed_components/morsemicro__halow/configs/sdkconfig.defaults.seeed_xiao_esp32s3-seeed_xiao_mm6108`
   - copied into `sdkconfig.defaults` directly. **Not independently verified against a real
     board** - if the physical module/wiring differs from Seeed's reference pairing, these pins
     or the BCF file name would need to change.
6. **Default partition table too small.** The linked binary (~1.58MB - HaLow SDK + firmware/BCF
   blobs + softAP + NAT + CoT relay + console) overflows the default 1MB app partition by ~600KB,
   and still doesn't fit ESP-IDF's built-in "large" preset (1500K). Added a custom `partitions.csv`
   with a 2MB factory partition + `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, matching the layout the
   component's own `dual_if` example (closest in scope - HaLow STA + native WiFi AP together) uses.
7. **`CONFIG_LWIP_IGMP` isn't a real Kconfig symbol** in this IDF version - IGMP is hardcoded on
   unconditionally in `lwipopts.h`. Removed the dead sdkconfig line; a real build had warned
   "unknown kconfig symbol" on it.
8. **The real `mmhalow_connect()` is async**, not blocking - confirmed from
   `managed_components/morsemicro__halow/examples/sta_connect/main/app_main.c` and `mmhalow.c`
   itself. `uplink_halow.c`'s `halow_sta_connect()` now wraps the real
   `mmwlan_sta_status_cb_t` callback in a semaphore to keep the existing
   connect-with-timeout/backoff loop shape, matching the pattern the official examples use.
9. **"Associated" isn't the same as "ready for NAT/CoT relay."** `mmhalow_init()` creates its
   netif via the same `ESP_NETIF_DEFAULT_WIFI_STA()` a normal esp_wifi STA would use (confirmed
   by reading `mmhalow.c` directly - the component has no public "get netif" accessor, so the
   netif handle is retrieved via `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")` instead), which
   means it runs a real DHCP client and fires `IP_EVENT_STA_GOT_IP`/`LOST_IP` just like esp_wifi.
   `uplink_halow.c`'s "connected" callback (the one that gates `ip_forward_nat_init`/
   `cot_relay_start` in `app_main.c`) now fires on that IP event, not on raw 802.11 association -
   using the earlier signal would have let NAT/CoT-relay code call `esp_netif_get_ip_info()`
   before DHCP finished, silently working against `0.0.0.0`.

## Web flasher + on-device management UI

Added after the initial build-verification pass, on request:

- **`main/web_ui.c`/`.html`**: `esp_http_server` serving a single self-contained HTML page
  (embedded via CMake `EMBED_FILES`, no separate filesystem partition) at the SoftAP's IP. REST
  endpoints (`GET`/`POST /api/config`, `POST /api/reboot`) read/write the *same* `gw_config_t` /
  `provisioning_save()` the `gwcfg-*` console commands use - it's a second transport onto
  identical logic, not a parallel config system. Passwords are never echoed back in `GET
  /api/config` responses; a blank password field on save means "keep current," not "clear it."
  Compiles clean (verified the same way as everything else - see above).
- **`docs/`**: a static [ESP Web Tools](https://esphome.github.io/esp-web-tools/) page
  (Web Serial, Chrome/Edge only) that flashes the firmware with placeholder config - it does not
  bake in Wi-Fi/HaLow credentials from the browser. Real config happens after flashing via the
  web UI above.
- **`.github/workflows/build-firmware.yml`**: builds firmware in the official `espressif/idf`
  Docker image (via `espressif/esp-idf-ci-action`, sidesteps the local-toolchain gotchas found
  above entirely) on every push to `main`, generates `manifest.json`, and deploys
  `docs/` + the built binaries to GitHub Pages.
  **Not yet exercised for real** - no push to `main` has triggered it yet in this environment, and
  **GitHub Pages needs a one-time manual enable** (repo Settings → Pages → Source: "GitHub
  Actions") before the deploy step will succeed; that's a web UI action, not something a
  commit can do.
- **Per-region firmware builds**: the workflow now runs a build matrix over `country-configs/*.defaults`
  (`US`, `CA`, `EU`, `GB`, `AU`, `NZ`, `JP`, `KR`, `IN`), baking a real `CONFIG_HALOW_COUNTRY_CODE`
  into each region's binary instead of shipping the `"??"` placeholder - it's a build-time value
  `gwcfg-*` can't set, so this can't be fixed at runtime. The flasher page picks which region's
  manifest to install from. This list isn't arbitrary - it's exactly the 9 regulatory domains
  `mmregdb.c` (in Morse Micro's `mm-iot-sdk`, upstream of `morsemicro/halow`) ships channel tables
  for; confirmed by reading that source directly, since neither the ESP-IDF component's Kconfig
  help text nor its docs enumerate the list. Other countries aren't buildable until Morse Micro
  ships regdb data for them. **Doesn't resolve open item 3 below** - which of these 9 (if any)
  actually matches the mesh Pi's own regulatory domain is still unconfirmed against real hardware.
- Bumped flash to the confirmed real 8MB and the app partition to 3MB (from 2MB) to give the
  larger binary (HTTP server + cJSON + embedded HTML) comfortable headroom.

Trade-off made deliberately, not by default: config is **not** baked into the flashed image from
the browser (which would mean reimplementing ESP-IDF's NVS binary format in JavaScript and
keeping it byte-exact with `gw_config_t` forever - fragile). First-time setup is "flash, connect
to the device's own SoftAP, use the web UI" instead - one config UI for both first boot and later
changes.

## What's implemented

| Module | File | Status |
|---|---|---|
| Local SoftAP + DHCP | `main/downlink_softap.c` | Implemented, compiles clean. Not yet flashed/tested on hardware. |
| HaLow STA uplink | `main/uplink_halow.c` | Implemented against the real `morsemicro/halow` API (see above), compiles clean. Exposes a four-state link state, RSSI, a blocking scan wrapper, and radio version info for bring-up. Not yet flashed/tested on hardware. |
| NAPT (uplink NAT) | `main/ip_forward_nat.c` | Implemented: DNS propagation into the SoftAP's DHCP offers, uplink as default route, `esp_netif_napt_enable()` on the downlink - all three, called once the uplink gets an IP. Not yet flashed/tested. |
| CoT multicast relay | `main/cot_relay.c` | Implemented: single socket joined to 239.2.3.1:6969 on both netifs, uses `IP_PKTINFO`/`recvmsg()` to identify arrival interface and avoid a forwarding loop. Not yet flashed/tested. |
| Provisioning (NVS + console) | `main/provisioning.c` | Implemented: `gwcfg-show` / `gwcfg-set-uplink` / `gwcfg-set-softap` / `gwcfg-set-node` / `gwcfg-save` / `gwcfg-reset` over the serial console (now on the right USB peripheral), NVS blob load/save, placeholder defaults. Not yet flashed/tested. |
| Web config UI | `main/web_ui.c`/`.html` | Implemented: `esp_http_server` + embedded HTML, GET `/api/status`, GET/POST `/api/config`, GET `/api/log`, POST `/api/scan`, POST `/api/reboot`, same NVS config as the console. SoftAP clients only; no authentication yet. Restyled to share the web flasher's design system (dark mode included). Not yet flashed/tested. |
| Status LED | `main/status_led.c` | Implemented: on-board GPIO21 LED blinks the uplink link state (radio failed / searching / associated-no-lease / up). The only instrument that needs neither cable nor phone. Not yet flashed/tested. |
| Factory reset | `main/factory_reset.c` | Implemented: 5s BOOT-button hold restores defaults and reboots, LED acknowledges at 1.5s. Runtime hold, not hold-at-power-on (that enters the ROM bootloader). Not yet flashed/tested. |
| Log ring buffer | `main/log_buffer.c` | Implemented: `esp_log_set_vprintf` tee into a 6KB RAM ring, served at `/api/log`. Chains to the previous handler so serial output is unaffected. Not yet flashed/tested. |
| App wiring | `main/app_main.c` | Brings up the log buffer, LED, factory-reset watcher, SoftAP, console and web UI immediately; brings up NAT + CoT relay once the uplink reports a DHCP-leased IP, retrying on the next reconnect if that fails. Skips the reconnect task entirely if the radio never initialized. Not yet flashed/tested. |
| Web flasher + CI | `docs/`, `country-configs/`, `.github/workflows/build-firmware.yml` | Implemented: ESP Web Tools page with a region picker + per-region GitHub Actions build/deploy matrix (`US`/`CA`/`EU`/`GB`/`AU`/`NZ`/`JP`/`KR`/`IN` - all 9 regdb-defined domains). **Not yet run for real** - needs GitHub Pages enabled (Settings → Pages → Source: GitHub Actions) and a push to `main` to exercise it. |

## Build-order checklist (DESIGN.md §8)

**Procedure for all of this now lives in [`BRINGUP.md`](BRINGUP.md)** - what to run at each step,
what a pass looks like, and how to tell identical-looking failures apart. This list stays as the
status tracker.

- [ ] **Step 0** - Confirm the Pi's HaLow radio config (`hostapd_s1g` AP mode, SSID/security).
      See `pi_side_reference.md` open items 1 and 3. `CONFIG_HALOW_COUNTRY_CODE` now defaults to
      `"US"` for local builds, but it still has to *match the Pi* - flash the matching region
      build. The `CONFIG_MM_*` pin/BCF config is a verbatim copy of the component's own
      `seeed_xiao_esp32s3-seeed_xiao_mm6108` config (see `HARDWARE.md`) and matches the confirmed
      hardware, but has still never been run against a physical board - `gwcfg-radio` is the check.
- [ ] **Step 1** - HaLow STA association works against that config. Code compiles and should be
      structurally correct (see items 8/9 above); unverified on real hardware. The status panel /
      `gwcfg-status` now distinguish this from step 2 explicitly (`searching` vs.
      `associated, no lease`); `gwcfg-scan` says whether the AP is even audible first.
- [ ] **Step 2** - DHCP lease + reachability confirmed (ping a Pi node; confirm the lease is
      visible from the Pi side). Blocked on step 1.
- [ ] **Step 3** - Local SoftAP + DHCP validated standalone (phones can join, get a lease). The
      natural first hardware test - doesn't depend on steps 0-2 at all.
- [ ] **Step 4** - NAPT validated (phone gets outbound mesh/internet reach). Needs a working
      uplink (steps 1-2) to test for real. Confirm translated source addresses actually appear on
      the mesh side - NAPT was on the wrong netif until the first review pass, and the failure mode
      is "associates fine, no connectivity."
      **Test name resolution separately from IP reachability**: DNS propagation into the SoftAP's
      DHCP offers was missing until this pass, and the two fail independently. A client that joined
      *before* the uplink came up holds a DNS-less lease until it renews - rejoin before concluding
      anything.
- [ ] **Step 4a** - **Plain multicast over HaLow validated, before involving the relay.** Confirm
      an IGMP join on the Morse Micro interface actually receives group traffic, and that the
      Pi-side mesh forwards 239.2.3.1 at all. Multicast over mesh routing is a classic silent-drop
      point and fails identically to a broken relay - test it in isolation so the two can't be
      confused. See `TECHNICAL_REVIEW.md` "For hardware bring-up".
- [ ] **Step 5** - CoT multicast relay validated (ATAK on a phone sees mesh CoT and vice versa).
      Needs a working uplink and step 4a to test for real.
- [ ] **Step 6** - Provisioning/config UX pass. Serial console (`gwcfg-*`) and an on-device web UI
      (`main/web_ui.c`, connect to the SoftAP and browse to its IP) both exist now, compile clean,
      neither flashed/tested on hardware yet. No captive-portal DNS redirect (visiting *any* URL
      auto-opens the config page) - user has to know to browse to the device's IP. See
      [`FEATURES.md`](FEATURES.md) item 5.
      **The web UI's status panel is the intended bring-up instrument for steps 1-5**: link state
      (in words, not a boolean), uplink RSSI, the DHCP-leased uplink IP, SoftAP client count,
      whether the CoT relay actually started, uptime, free heap, and the baked-in country code -
      plus a HaLow scan button and a device-log view. The LED covers the same states when there's
      no phone attached either.
- [ ] **Step 7** - Web UI authentication. Blocks shipping and blocks OTA update delivery. Design
      is settled (forced password change on first use, challenge-response so the password never
      crosses the wire, serial-console recovery) - see `TECHNICAL_REVIEW.md` "Deferred: web UI
      authentication" and [`FEATURES.md`](FEATURES.md) item 1. Not started.

## Open questions

Tracked in detail in [`pi_side_reference.md`](pi_side_reference.md):

1. Pi's HaLow AP security mode - real option set is open/OWE/SAE (corrected from the original
   open/PSK/SAE draft), value still unconfirmed against a live Pi.
2. DHCP scope/lease ownership (the associated Pi vs. centralized by `openmanetd`).
3. Regulatory/country code the Pi's HaLow radio uses - **this one does block real association**,
   unlike 1/2/4, since it's a build-time XIAO-side setting (`CONFIG_HALOW_COUNTRY_CODE`), not
   runtime-provisioned.
4. Mesh-point + AP concurrency on one radio, once a second Pi joins (doesn't block a
   single-Pi build).

## Pre-hardware technical review (2026-08-05)

Full findings and verification detail in [`TECHNICAL_REVIEW.md`](TECHNICAL_REVIEW.md). Summary of
what changed in this pass - all fixes are in, none are hardware-validated:

| Severity | Finding | Status |
|---|---|---|
| Critical | CoT relay matched `IP_PKTINFO.ipi_addr` (packet destination = the multicast group) instead of `ipi_ifindex`, so it dropped every datagram while logging success | Fixed |
| Critical | NAPT enabled on the uplink netif; ESP-IDF requires it on the SoftAP netif, with the uplink as default route | Fixed |
| High | Relay could ping-pong its own transmissions once arrival detection worked | Fixed - `IP_MULTICAST_LOOP` off + own-source drop |
| High | Unauthenticated `/api/config` + `/api/reboot` reachable from the whole mesh, not just SoftAP clients | Fixed - peer address must be on the SoftAP subnet |
| High | Invalid config (e.g. 4-char WPA2 passphrase) could be saved, killing the SoftAP management path at next boot | Fixed - shared `provisioning_validate()` |
| Medium | Uplink could wait for a DHCP lease forever | Fixed - bounded wait, dhcpc restart, then re-associate |
| Medium | Fast association failures still burned the full 15s connect timeout | Fixed - state callback signals both outcomes |
| Medium | Socket leaked on `cot_relay_start()` error paths | Fixed |
| Medium | Live `gw_config_t` shared across three tasks without locking | Fixed - `provisioning_config_lock()` |
| Medium | Unbounded SSID/passphrase copies into the mmwlan config struct | Fixed - bounded by destination field |
| Medium | `esp_restart()` raced its own HTTP response; unchecked cJSON/semaphore returns | Fixed |
| Medium | NVS blob had no version stamp - a layout change would silently reset deployed units | Fixed - magic + version |
| Perf | Assessed; no meaningful pre-hardware gains (HaLow-over-SPI is the ceiling). Added a 64K coredump partition for bring-up observability instead | Done |
| Open | **OTA deferred pending final binary size** - single 3MB factory slot means USB-only updates today | Decision pending |

**OTA was split in two; the layout half is now done.** The binary measured **1.64MB**
(`0x1A4620`, real build), so two 3MB slots fit within 8MB flash while keeping the previous app
ceiling unchanged - capacity was never the constraint, so there was nothing to learn by waiting.
The partition table is now dual-OTA (`ota_0`/`ota_1` + `otadata`, ~1.81MB still unallocated),
adopted immediately because retrofitting a table onto deployed units costs a USB cable per unit.

**The update mechanism is still deferred, deliberately.** An OTA endpoint means anyone who can
reach the web UI can replace the firmware, and that UI still has no authentication - the SoftAP
passphrase is the only boundary. **Auth lands before any firmware-upload path exists.** Rollback
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) is likewise off until the app calls
`esp_ota_mark_app_valid_cancel_rollback()`, or it would revert every boot. Sequence and detail in
[`TECHNICAL_REVIEW.md`](TECHNICAL_REVIEW.md) "Deferred: OTA".

⚠️ **Flash offsets changed.** The app is now at `0x20000` (was `0x10000`), and `otadata` at
`0x10000` needs the generated `ota_data_initial.bin`. Anything that flashes this firmware - the
web flasher manifest, any local scripts, any notes - must match, or the board flashes "successfully"
and doesn't boot. CI now asserts its manifest offsets against the build's own `flash_args` and
fails the job on disagreement.

## Known v1 limitations (intentional, not bugs)

- `app_main.c`'s `on_uplink_state()` only initializes NAT/CoT relay on the **first successful**
  uplink connect. If a later reconnect gets a different DHCP-leased IP, they are not re-initialized
  against it. Documented in a comment there; fine for v1, worth revisiting if lease changes turn
  out to happen in practice. (A *failed* init does now retry on the next reconnect - that was a
  bug, fixed in the readiness pass, and is a different thing from the lease-change limitation.)
- No inbound-unicast-to-a-specific-phone routing (NAT-only for v1, per `DESIGN.md` §4.3). Static
  routing on the gateway Pi is the suggested v2 fix if needed.
- No self-beacon (GPS/battery/status CoT) yet - `cot_relay_inject()` exists as the generic
  send primitive `DESIGN.md` §5.5 asks for, but nothing calls it yet.
- The `CONFIG_MM_BCF_FILE`/pin config was copied from the component's reference config for this
  exact board pairing (Seeed XIAO ESP32S3 + Seeed XIAO WM6108 - the hardware in use, confirmed)
  and never checked against a physical board - if the real hardware differs even slightly
  (different HaLow HAT, different wiring), this needs regenerating from
  `managed_components/morsemicro__halow/configs/`. `gwcfg-radio` is the one-command check; see
  [`HARDWARE.md`](HARDWARE.md) for the full pin table and its provenance.
- The web UI (`main/web_ui.c`) has no authentication - anyone who can join the SoftAP (i.e. who
  knows its password) can reconfigure the gateway. Acceptable for v1 given the SoftAP itself
  already requires a password by default, but worth revisiting if that's not enough isolation for
  a real deployment. As of the review pass this is now genuinely limited to SoftAP clients: the
  handlers refuse requests from outside the SoftAP subnet, so it is no longer mesh-wide. That is
  subnet-based *authorization*, not authentication - it does not defend against a device already
  associated to the SoftAP.
- No OTA *delivery* yet. The partition layout is OTA-capable (dual 3MB slots), but nothing
  performs updates, so firmware changes still need a USB cable in practice. Blocked on web UI
  authentication by choice, not by effort - see [`TECHNICAL_REVIEW.md`](TECHNICAL_REVIEW.md)
  "Deferred: OTA" for the ordered list of what has to land.
- ~~The DHCP-failure recovery path re-calls `mmhalow_connect()` rather than doing a true
  radio-level disconnect first.~~ **Resolved.** `mmhalow_disconnect()` does exist in the
  component's public `mmhalow.h` (implemented as `mmwlan_sta_disable()`); the recovery path now
  disconnects before re-associating.
