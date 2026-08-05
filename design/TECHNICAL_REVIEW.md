# Technical Review - Pre-Hardware Pass

Findings from a full read-through of the firmware before moving to hardware testing, and what
was done about each. Read alongside [`PROGRESS.md`](PROGRESS.md) (status tracker) and
[`DESIGN.md`](DESIGN.md) (the design itself).

- **Branch:** `claude/technical-review-performance-mmq12j`
- **Reviewed at:** commit `b5873ea`
- **Last updated:** 2026-08-05

## Why this document exists

Two of the findings below are bugs that would have presented on hardware as *radio* problems -
"the HaLow link is flaky", "ATAK doesn't see anything" - while the actual defect was in code that
compiled clean and logged success. Both were found by checking this firmware's assumptions
against upstream ESP-IDF and lwIP source, not by reasoning about the code in isolation. That's the
lesson worth keeping: **a compiling ESP-IDF build proves internal consistency, not that an API
means what the call site assumes it means.**

Each finding records how it was verified, so the reasoning can be re-checked rather than trusted.

---

## Critical - would have failed silently on hardware

### 1. CoT relay dropped every datagram (`main/cot_relay.c`) - FIXED

**What was wrong.** The relay identified which interface a multicast datagram arrived on by
comparing `IP_PKTINFO`'s `ipi_addr` against each netif's own unicast address. lwIP fills
`ipi_addr` from the packet's **destination** address, which for this relay is always the CoT
multicast group (239.2.3.1) - so it never equals an interface address. Every datagram fell
through to the "arrived via neither known interface - drop" branch.

**Why it was hard to see.** Nothing errors. The socket binds, both group joins succeed, the task
starts, and `CoT relay joined 239.2.3.1:6969 on both interfaces` is logged. The relay simply
forwards nothing, which on hardware looks like a mesh or ATAK problem.

**How it was verified.** esp-lwip `src/api/sockets.c`, the `LWIP_NETBUF_RECVINFO` control-message
path:

```c
pkti->ipi_ifindex = buf->p->if_idx;
inet_addr_from_ip4addr(&pkti->ipi_addr, ip_2_ip4(netbuf_destaddr(buf)));
```

`netbuf_destaddr()` is the datagram's destination, and `ipi_ifindex` is the arrival interface.

**Fix.** Match on `ipi_ifindex` against `esp_netif_get_netif_impl_index()`, resolved once at start
rather than per packet (interface indices are stable for a netif's lifetime). `cot_relay_start()`
now also fails loudly if the two indices are unresolvable or identical, since that would make
arrival detection meaningless.

### 2. NAPT was enabled on the wrong interface (`main/ip_forward_nat.c`) - FIXED

**What was wrong.** `esp_netif_napt_enable()` was called on the **uplink** netif. It belongs on
the **SoftAP** netif. SoftAP client traffic would have left the HaLow radio with untranslated
`192.168.50.x` source addresses - unroutable on the mesh, presenting as "phone associates to the
gateway but has no connectivity."

**How it was verified.** Three independent upstream sources agree:

1. ESP-IDF `examples/wifi/softap_sta/main/softap_sta.c` at the pinned v5.5.1 tag:
   ```c
   /* Enable napt on the AP netif */
   if (esp_netif_napt_enable(esp_netif_ap) != ESP_OK) { ... }
   ```
2. That example's README: *"With NAPT enabled on the softAP interface and the station interface
   set as the default interface this example can be used as Wifi nat router."*
3. `components/esp_netif/lwip/esp_netif_lwip.c` - the control API sets `napt = 1` on precisely the
   netif it is handed, and **clears it on every other netif**, so only one interface can carry it
   at a time. There is no "pick the upstream one" logic to rely on.

**Fix.** NAPT moved to the downlink netif; the signature is now
`ip_forward_nat_init(downlink_netif, uplink_netif)` so a caller cannot get the order wrong by
passing a single ambiguous "the netif" argument. The inverted explanation in `ip_forward_nat.h`
was corrected - it had documented the wrong model, which is why the bug read as intentional.

Source (3) also surfaced a requirement the original code never satisfied: the uplink must be the
**default route** netif. `esp_netif` would normally elect the STA netif on route priority, but
that netif is created by a third-party component, so `ip_forward_nat_init()` now sets it
explicitly instead of depending on that.

---

## High - fixed before field use

### 3. Relay loop prevention was incomplete - FIXED

Once finding 1 was fixed, a real risk appeared that the broken code had masked: the relay is
joined to the group on both interfaces it transmits on, so its own datagrams could be re-received
and forwarded back out the other interface - a self-sustaining loop. Two defences added:
`IP_MULTICAST_LOOP` explicitly disabled (rather than trusting the stack default), and any datagram
whose **source** address matches either local interface address is dropped. The second also covers
`cot_relay_inject()` when the self-beacon eventually lands.

### 4. Management API was reachable from the whole mesh - FIXED

`httpd_start()` binds all interfaces. Once the uplink came up, unauthenticated
`POST /api/config` and `POST /api/reboot` were reachable from every node on the mesh, not just
SoftAP clients. `PROGRESS.md` described the threat model as "anyone who can join the SoftAP",
which was narrower than reality.

Every handler now refuses requests whose peer address falls outside the SoftAP's subnet
(`getpeername()` on the request socket, masked against the SoftAP netif's own IP/netmask; IPv4
and IPv4-mapped-IPv6 peers both handled). It fails closed, and `web_ui_start()` now requires the
SoftAP netif rather than accepting NULL - starting a server that would refuse everything is worse
than not starting.

This is subnet-based authorization, not authentication. It restores the intended boundary; it does
not defend against a device already associated to the SoftAP. Real auth remains a v2 item.

### 5. Invalid config could brick the management path - FIXED

WPA2 requires an 8-63 character passphrase. Both the web UI and console would happily save a
shorter one; `esp_wifi_set_config()` then fails at next boot, the SoftAP never comes up, and the
primary management path is gone - recovery is USB console only. Channel, CoT group, and port were
similarly unvalidated.

Added `provisioning_validate()`, shared by the console, the web UI, and the NVS load path so all
three agree on what "valid" means:

| Field | Rule |
|---|---|
| `node_id`, both SSIDs | non-empty |
| SoftAP passphrase | empty (open) **or** 8-63 chars |
| SoftAP channel | 1-13 (14 is Japan-only/802.11b-only and rejected by esp_wifi under most country settings) |
| `max_connections` | <= 15 |
| Custom subnet fields | parse as valid IPv4 |
| CoT group | valid IPv4 **and** within 224.0.0.0/4 |
| CoT port | non-zero |
| Uplink SAE | passphrase present |

No length floor is asserted for the SAE passphrase - unlike WPA2-PSK, SAE does not specify one.
`provisioning_save()` refuses to write a config that fails validation, and `provisioning_load()`
falls back to defaults rather than booting one, so a blob written by an older build can't strand a
unit either. The web UI surfaces the specific reason rather than a generic 400.

---

## Medium - robustness, all fixed

- **Uplink could wait for DHCP forever** (`uplink_halow.c`). Association succeeding while DHCP
  never completes is a realistic bring-up failure, and the poll loop had no bound - it would sit
  there indefinitely, never retrying, never reporting. Now bounded: wait
  `DHCP_LEASE_TIMEOUT_MS`, restart the DHCP client in place (fixes the transient cases cheaply),
  and on continued failure fall back to re-associating.
  *Constraint worth knowing:* the re-associate path reuses `mmhalow_connect()`, which the existing
  reconnect loop already assumed is re-callable. A true radio-level disconnect would be cleaner,
  but no `mmhalow_disconnect()` could be confirmed against the real component headers, and this
  fix deliberately uses only APIs already exercised by this codebase rather than a guessed one.
  **Confirm against the component headers during bring-up.**
- **Slow-failing connect attempts** (`uplink_halow.c`). `mm_sta_state_cb` never signalled on
  `MMWLAN_STA_DISABLED`, so a fast association failure still burned the full 15 s timeout before
  retrying. It now signals both outcomes, with `halow_sta_connect()` re-checking `s_associated`
  to tell success from failure.
- **Socket leaked on `cot_relay_start()` error paths.** The `ESP_RETURN_ON_ERROR` exits after
  `socket()` succeeded returned without closing it, leaving `s_sock` pointing at a
  half-configured socket that `cot_relay_inject()` would still have used. Consolidated onto a
  single cleanup path; re-entry is now rejected explicitly.
- **Unsynchronized shared config.** The live `gw_config_t` is touched by the console REPL, the
  httpd task, and `app_main`. Added `provisioning_config_lock()`/`unlock()` and applied it to
  every read-modify-write, including the snapshots `app_main` hands to the radio and the relay.
- **Unprotected `setsockopt`+`sendto` pair.** `send_via()` retargets a shared socket's multicast
  interface then sends; concurrent callers could interleave. Now mutex-protected - this matters
  more once `cot_relay_inject()` has a caller.
- **Unbounded copies into the mmwlan config struct.** SSID and passphrase were copied by
  `strlen()` into fields whose size comes from a third-party header. Now bounded by the
  destination field and rejected with a clear error if oversized.
- **Small crash paths.** `xSemaphoreTake(NULL, ...)` if semaphore creation failed;
  `cJSON_PrintUnformatted()` return value unchecked. Both guarded.
- **Reboot raced its own HTTP response.** `esp_restart()` was called inline, resetting the CPU
  before the response drained - the browser saw a connection reset. Deferred via a one-shot
  `esp_timer`.
- **NVS blob had no version stamp.** Any future change to `gw_config_t`'s layout would have
  silently discarded a deployed unit's config back to placeholder defaults. Added magic +
  version, checked on load; size alone was insufficient since a field can change meaning without
  changing size.

---

## Performance

**Assessed, and deliberately not optimized.** There are no meaningful firmware-side performance
gains available before hardware testing:

- The MM6108 HaLow link over SPI is the throughput ceiling (single-digit Mbps at best, far less at
  range). The S3's lwIP NAT path handles that with headroom to spare.
- The CoT relay's per-packet cost is irrelevant at CoT message rates. The ifindex fix happens to
  remove two `esp_netif_get_ip_info()` calls per packet anyway, so the correct version is also the
  cheaper one.

If TCP-through-NAT throughput disappoints on hardware, the knobs are
`CONFIG_LWIP_TCP_WND_DEFAULT` / `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` and the Wi-Fi buffer counts -
but **measure first**. Pre-tuning against a link whose real characteristics are still unknown
would be guessing.

One change was made in this area, and it buys observability rather than speed: a **64K coredump
partition** with flash coredumps enabled. Bring-up means crashes happen unattended and away from a
terminal; without it a panic leaves nothing behind. `idf.py coredump-info` reconstructs the
backtrace after the fact. Flash cost is trivial against 8MB.

---

## Deferred: OTA

**Status: open decision, deliberately not made yet.**

The current table has a single 3MB `factory` slot, so every firmware update on a deployed unit
requires a USB cable. 8MB of flash comfortably fits a dual-OTA layout (two ~3MB app slots plus
`otadata`), and retrofitting the partition table onto already-flashed units is itself a
cable-update event - which is exactly the cost OTA exists to avoid.

**Why it's deferred rather than done:** the final binary size isn't settled. Features still to
land (self-beacon, any web UI growth, possible auth) all add to it, and the app-slot size chosen
now sets the ceiling for every future update. Committing to a slot size before knowing where the
binary lands risks either wasting space or capping growth.

**Measured size as of this review** (real `idf.py build`, ESP-IDF v5.5.1, US region):

| | Bytes | |
|---|---|---|
| `xiao_halow_gateway.bin` | `0x1A4620` | **1.64 MB** |
| Current `factory` slot | `0x300000` | 3 MB (45% free) |
| Flash | | 8 MB (well under half allocated) |

That number makes the decision less open than it looks. A dual-OTA layout at **3MB per slot** -
i.e. keeping today's ceiling exactly, losing nothing - costs roughly 6.1MB of the 8MB once
`otadata`, `nvs`, `phy_init`, and `coredump` are counted. It fits, with room to spare, *today*.
Two 2MB slots would also hold the current binary (with ~18% headroom) if more room were wanted
elsewhere, but there's no reason to take that tradeoff at 8MB.

**What to decide it on:** the remaining question is not "does it fit" but "has the feature set
stopped moving." If the answer is yes, dual 3MB OTA is a safe, non-compromising choice and should
just be done. Re-measure after the self-beacon and any auth work land, then switch in one
deliberate change. `partitions.csv` leaves the flash tail unallocated specifically so this stays
possible.

**The cost of waiting** is that any unit deployed before the switch needs a physical cable to move
to an OTA-capable table. If units are going out to people who can't easily bring them back, make
the OTA decision *before* they ship, even if the binary size is still moving.

---

## For hardware bring-up

### Verify multicast over HaLow early, and in isolation

Independent of finding 1: it is worth confirming that an IGMP join on the Morse Micro interface
actually results in group traffic being delivered, and that the Pi-side mesh forwards 239.2.3.1 at
all. Multicast over mesh routing is a classic silent-drop point, and it fails in exactly the same
way a broken relay does. Test it with a plain multicast send/receive **before** wiring the relay
into the picture, so radio/mesh behaviour and relay behaviour can't be confused for one another.
This has been added to the build-order checklist in `PROGRESS.md` as step 4a.

### Confirm during bring-up

- Whether the component exposes a disconnect/deinit API, which would give the DHCP-failure path a
  cleaner recovery than re-calling `mmhalow_connect()` (see the Medium section).
- That `CONFIG_MM_*` pin/BCF values match the physical board - still copied from Seeed's reference
  pairing and never checked against hardware.
- The real `CONFIG_HALOW_COUNTRY_CODE` for the deployment, and whether it matches the Pi's
  regulatory domain (`PROGRESS.md` open question 3 - the one that genuinely blocks association).

---

## Not changed, and why

- **No captive-portal DNS redirect.** Still a real UX gap (users must know to browse to the
  device's IP), but it's a feature, not a defect, and it was out of scope for this pass.
- **No web UI authentication.** Finding 4 restored the intended SoftAP-only boundary. Real auth
  is a larger design decision - where the credential lives, how it's provisioned, what happens
  when it's forgotten - and shouldn't be improvised here.
- **Single-uplink-IP assumption on reconnect.** `app_main.c`'s `on_uplink_state()` still only
  initializes NAT and the relay on the first connect. Documented as a known v1 limitation; whether
  it matters depends on whether lease changes happen in practice, which hardware testing will
  answer.
- **No `esp_wifi` bandwidth/PHY tuning.** See Performance - measure first.
