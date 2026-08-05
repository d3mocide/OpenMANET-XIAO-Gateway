# Progress / Roadmap

Status tracker for the XIAO HaLow gateway firmware. Read this first if you're picking the
project back up (human or agent) - it says what's real, what's verified, and what to do next.
Read [`DESIGN.md`](DESIGN.md) for the full design and [`pi_side_reference.md`](pi_side_reference.md)
for Pi-side facts before making changes. Update this file's checklist whenever you finish a step
below or learn something that changes it.

- **Branch:** `claude/xaio-client-node-design-lkv9og`
- **PR:** https://github.com/d3mocide/OpenMANET-S3-Client/pull/1
- **Last updated:** 2026-08-05

## Status at a glance

`idf.py build` **passes end-to-end** against ESP-IDF v5.5.1 with the real `morsemicro/halow`
component fetched from the ESP Component Registry - verified by actually running the build in a
throwaway ESP-IDF checkout, not just by reading code. The binary links and fits its partition
(21% headroom). All of `main/*.c`, including the HaLow STA uplink, compiles clean against the
real component headers.

**What that build pass does *not* mean:** nothing has been flashed to real hardware. A compiling
build proves the code is internally consistent against the real APIs; it doesn't prove the HaLow
radio actually associates, DHCP actually completes, or NAT/CoT relay actually pass traffic. See
"Build-order checklist" below for what's still real-hardware-only.

## How this was verified (so it can be redone)

No CI is wired up yet, so this was done by hand in a scratch environment:

```sh
git clone -b v5.5.1 --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf && git submodule update --init --recursive --depth 1
./install.sh esp32s3   # needs libusb-1.0-0 (apt) for openocd-esp32, else it
                        # silently aborts before finishing the Python venv setup
source export.sh
cd /path/to/OpenMANET-S3-Client
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

## What's implemented

| Module | File | Status |
|---|---|---|
| Local SoftAP + DHCP | `main/downlink_softap.c` | Implemented, compiles clean. Not yet flashed/tested on hardware. |
| HaLow STA uplink | `main/uplink_halow.c` | Implemented against the real `morsemicro/halow` API (see above), compiles clean. Not yet flashed/tested on hardware. |
| NAPT (uplink NAT) | `main/ip_forward_nat.c` | Implemented via `esp_netif_napt_enable()`, called once the uplink gets an IP. Not yet flashed/tested. |
| CoT multicast relay | `main/cot_relay.c` | Implemented: single socket joined to 239.2.3.1:6969 on both netifs, uses `IP_PKTINFO`/`recvmsg()` to identify arrival interface and avoid a forwarding loop. Not yet flashed/tested. |
| Provisioning (NVS + console) | `main/provisioning.c` | Implemented: `gwcfg-show` / `gwcfg-set-uplink` / `gwcfg-set-softap` / `gwcfg-set-node` / `gwcfg-save` / `gwcfg-reset` over the serial console (now on the right USB peripheral), NVS blob load/save, placeholder defaults. Not yet flashed/tested. |
| App wiring | `main/app_main.c` | Brings up SoftAP + console immediately; brings up NAT + CoT relay once the uplink reports a DHCP-leased IP. Not yet flashed/tested. |

## Build-order checklist (DESIGN.md §8)

- [ ] **Step 0** - Confirm the Pi's HaLow radio config (`hostapd_s1g` AP mode, SSID/security).
      See `pi_side_reference.md` open items 1 and 3. Also set the real `CONFIG_HALOW_COUNTRY_CODE`
      (currently placeholder `"??"`) and confirm/adjust the `CONFIG_MM_*` pin/BCF config against
      the physical board before flashing - neither has been checked against real hardware.
- [ ] **Step 1** - HaLow STA association works against that config. Code compiles and should be
      structurally correct (see items 8/9 above); unverified on real hardware.
- [ ] **Step 2** - DHCP lease + reachability confirmed (ping a Pi node; confirm the lease is
      visible from the Pi side). Blocked on step 1.
- [ ] **Step 3** - Local SoftAP + DHCP validated standalone (phones can join, get a lease). The
      natural first hardware test - doesn't depend on steps 0-2 at all.
- [ ] **Step 4** - NAPT validated (phone gets outbound mesh/internet reach). Needs a working
      uplink (steps 1-2) to test for real.
- [ ] **Step 5** - CoT multicast relay validated (ATAK on a phone sees mesh CoT and vice versa).
      Needs a working uplink to test for real.
- [ ] **Step 6** - Provisioning/config UX pass. Serial console (`gwcfg-*`) exists; no onboard
      config portal (e.g. captive portal / BLE) has been built - console-only is the current
      state, which DESIGN.md §5.6 calls a valid "standard ESP-IDF pattern" option.

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

## Known v1 limitations (intentional, not bugs)

- `app_main.c`'s `on_uplink_state()` only initializes NAT/CoT relay on the **first** uplink
  connect. If a later reconnect gets a different DHCP-leased IP, they are not re-initialized
  against it. Documented in a comment there; fine for v1, worth revisiting if lease changes turn
  out to happen in practice.
- No inbound-unicast-to-a-specific-phone routing (NAT-only for v1, per `DESIGN.md` §4.3). Static
  routing on the gateway Pi is the suggested v2 fix if needed.
- No self-beacon (GPS/battery/status CoT) yet - `cot_relay_inject()` exists as the generic
  send primitive `DESIGN.md` §5.5 asks for, but nothing calls it yet.
- The `CONFIG_MM_BCF_FILE`/pin config was copied from the component's reference config for this
  exact board pairing (Seeed XIAO ESP32S3 + Seeed XIAO WM6108) and never checked against a
  physical board - if the real hardware differs even slightly (different HaLow HAT, different
  wiring), this needs regenerating from `managed_components/morsemicro__halow/configs/`.
