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

**Status: partition layout DONE; update delivery still deferred.**

The decision was split in two, because the two halves have opposite risk profiles:

- **Part A - the partition layout (done).** Pure layout: no new code, no new attack surface, no
  runtime behaviour change. This is the half that gets expensive to defer, because retrofitting a
  partition table onto already-deployed units costs a USB cable per unit - exactly the cost OTA
  exists to remove. Now a dual-OTA table with two 3MB slots, keeping the previous app ceiling
  unchanged.
- **Part B - the update mechanism (still deferred).** This is where the risk is, and it is a
  direct consequence of finding 4 above. An OTA endpoint means *anyone who can reach the web UI
  can replace the firmware*. That UI still has no authentication - the SoftAP passphrase is the
  only boundary. Promoting "can change my config" to "can replace my firmware" on an
  unauthenticated endpoint is an escalation, not an increment. **Authentication must land before
  any firmware-upload path exists.**

Also intentionally not enabled yet: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. With rollback on, an
app that never calls `esp_ota_mark_app_valid_cancel_rollback()` gets reverted on every boot. That
call belongs with Part B, so turning rollback on now would break booting for no benefit.

### The layout that was adopted

```
nvs        0x009000    24K
phy_init   0x00F000     4K
otadata    0x010000     8K
ota_0      0x020000     3M   <- app lives here now, NOT 0x10000
ota_1      0x320000     3M
coredump   0x620000    64K
                       ---
total      0x630000   6.19MB of 8MB, ~1.81MB unallocated
```

No `factory` partition: at 3MB per slot a third app copy needs 9MB and doesn't fit. With blank or
invalid otadata the bootloader falls back to `ota_0`, which is where a fresh flash writes. ESP-IDF's
own reference tables keep a factory slot only because they assume 1MB apps.

**The trap this introduces:** the app moved from `0x10000` to `0x20000`, and `otadata` at
`0x10000` must receive the generated `ota_data_initial.bin`. The web flasher's manifest
(`.github/workflows/build-firmware.yml`) encodes both. A mismatch produces a flash that reports
success and then doesn't boot - so the workflow now asserts its offsets against the build's own
`flash_args` and fails the job on disagreement, rather than trusting a hand-maintained heredoc.

### The original analysis, for the record

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

**What it was decided on:** capacity turned out not to be the constraint - two 3MB slots fit with
~1.81MB to spare while giving up nothing, so waiting for the feature set to settle would have
bought no information that could change the slot size. The layout was adopted immediately; the
delivery mechanism, which *does* depend on unfinished work (authentication), was not.

**Still to do for Part B**, in order:

1. Authentication on the web UI. Nothing else should ship first.
2. An update path - `esp_https_ota` pulling from a known URL, or an authenticated upload endpoint.
3. `esp_ota_mark_app_valid_cancel_rollback()` in the app, then enable
   `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.
4. A real end-to-end update test on hardware, including a deliberately bad image, before any of it
   is relied on in the field.

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

## Deferred: web UI authentication

**Status: designed, not built.** Decisions recorded here so the next pass doesn't relitigate them.

Finding 4 restored the intended boundary (SoftAP clients only) but did not add authentication -
association with the SoftAP is still the only credential. That is acceptable for bench testing and
is *not* acceptable for shipping, and it blocks OTA update delivery outright: an upload endpoint
without auth means anyone who can reach the UI can replace the firmware.

### No hardcoded default password

Shipping a fixed default is both bad practice and, for a device sold or distributed, likely
non-compliant: California SB-327 and the UK PSTI Act each require either a unique per-device
credential or a forced change at setup. This board has no screen and no per-unit labelling step,
so per-device randomness can't be communicated to the user.

**Decision: forced change on first use.** Ship with a known default, and refuse to serve anything
except the "set a password" screen until it has been changed. The admin password must be
changeable at any time thereafter, exactly like the Wi-Fi passphrase.

### No TLS

No CA will issue a certificate for `192.168.50.1`, and a self-signed certificate trains users to
click through browser warnings - which is worse than no TLS, because it erodes the one signal that
matters elsewhere. TLS also costs RAM on a device already running NAT and the CoT relay.

**Decision: no TLS.** WPA2 on the SoftAP is the transport protection.

### The password still must not cross the wire in cleartext

With WPA2-PSK, anyone who knows the AP passphrase can decrypt other clients' traffic. So a
form-posted password is exposed to everyone who can already join the AP - which matters here,
because a team may share the Wi-Fi passphrase without every member being an administrator.

**Decision: challenge-response.** Server issues a nonce; the client returns
`HMAC(stored_key, nonce)`; the password itself never transits.

**Constraint that shapes the implementation:** `crypto.subtle` is only exposed in *secure
contexts*, and `http://192.168.50.1` is not one (only `localhost` is treated as trustworthy over
plain HTTP). WebCrypto is therefore unavailable and a small SHA-256/HMAC implementation must be
bundled into the embedded page - roughly 2KB. This was accepted deliberately; do not "simplify" it
back to `crypto.subtle` later, it will silently be `undefined` on the device.

### Remaining design points

- **Storage:** PBKDF2-HMAC-SHA256 with a per-device random salt in NVS (mbedtls is already
  linked). Never store the password itself. Tune iteration count against the S3 - logins are rare,
  so err high.
- **Sessions:** `esp_random()` tokens held in RAM only (they should not survive a reboot), a small
  fixed-size session table, idle timeout, `HttpOnly` + `SameSite=Strict` cookies for CSRF.
- **Brute force:** lockout or backoff after repeated failures - the attacker here is already on
  the LAN.
- **Recovery is mandatory.** A forgotten password must not brick a unit. The serial console is the
  escape hatch (`gwcfg-reset-auth`), which is physically-present-only and therefore the right
  trust model. Document it prominently; an undocumented recovery path is the same as none.

## Local subnet: why it changed, and what's still open

### The old default was never a decision

`192.168.50.0/24` traces to the first scaffold commit. `DESIGN.md` §4.2 only says the SoftAP
should have "its own subnet", and the network diagram writes it as *"e.g. 192.168.50.0/24"* - an
illustration that got copied into `provisioning_get_defaults()` and never revisited.

It was also, on inspection, an unusually poor choice: **ASUS consumer routers ship with
192.168.50.1 as a default LAN address.** A gateway whose uplink ever routed through such a
network would treat those addresses as on-link and quietly fail to forward.

**Now `172.16.41.0/24`:** disjoint from OpenMANET's `10.41.0.0/16` mesh, inside a range consumer
gear essentially never uses, held at `172.16.x` specifically because Docker allocates bridge
networks from `172.17`-`172.31`, and numbered `41` so it visually rhymes with the mesh.

### It is now actually configurable

The fields existed in `gw_softap_config_t` and were even validated - but nothing could set them.
`gwcfg-set-softap` took only ssid/psk/channel, and so did the web UI's POST handler. The subnet
was effectively compile-time-only. Both surfaces now expose it (`gwcfg-set-subnet <ip> [netmask]`
and two fields in the web UI).

Validation was correspondingly shallow - it checked only that the three strings parsed as
addresses. It now rejects non-contiguous masks, network/broadcast addresses, gateways outside
their own subnet, reserved ranges (`0/8`, `127/8`, `224/4+`, `169.254/16`), and subnets too small
for the configured client count. `max_connections` is additionally capped at
`CONFIG_LWIP_DHCPS_MAX_STATION_NUM`, because esp_wifi would otherwise associate more stations
than the DHCP server has leases for - producing clients that associate and then sit with no
address.

A separate runtime check (`provisioning_check_runtime_conflict()`) rejects a local subnet that
would swallow the mesh address the uplink currently holds. Static validation deliberately doesn't
know about liveness, so this is its own function, called from the two interactive surfaces but
not from the NVS load path.

### Multi-gateway: what NAT does and does not save you from

Two gateways on one Pi, both on the same local subnet:

- **Unicast is fine.** Each gateway NATs its clients behind its own distinct mesh lease, and the
  two SoftAPs are separate L2 domains that never see each other's ARP. Duplicate `172.16.41.1` is
  no worse than two houses both running a router at `192.168.1.1`.
- **ATAK CoT is not fine, and this is the sharp edge.** ATAK's CoT `<contact>` element carries an
  `endpoint` attribute holding the sender's *own* address. The relay re-sources the IP header but
  forwards the payload byte-for-byte - it has no idea there's an address inside the XML. With one
  gateway that is the already-known "no inbound unicast" limitation: peers see an unroutable
  private address and direct chat/file transfer fails. With **two gateways sharing a subnet it
  gets worse than failure**: a device behind gateway B that dials `172.16.41.23` will connect
  successfully - to *B's own* client at that address, a different person entirely. Silent
  mis-delivery rather than a clean error.

  **Confirm this on hardware** by capturing a real CoT multicast datagram and inspecting the
  `endpoint` attribute. The schema is documented behaviour, but it has not been observed on this
  setup.

  Giving each gateway a distinct subnet does not fix the underlying problem - peers still can't
  reach those addresses - but it converts silent mis-delivery back into a clean failure, which is
  a much better place to be. The real fix is architectural: the routed/no-NAT option in
  `DESIGN.md` §4.3. Rewriting addresses inside CoT payloads in the datapath is the obvious
  alternative and should be resisted; XML mutation in a forwarding hot path is a liability.

### Long-term goal: MAC-derived subnet + captive-portal DNS

Two gateways colliding is currently prevented by an operator remembering to change a setting.
The durable fix is to derive the third octet from the device MAC (e.g. `172.16.<mac>.1/24`) so
two units effectively cannot collide by accident.

The blocker is discovery: a user has no way to know which address to browse to, and there is no
screen or label. That makes this dependent on the **captive-portal DNS redirect** already listed
as a gap in `PROGRESS.md` step 6 - once any URL redirects to the config page, the device's actual
address stops needing to be known, and per-device subnets become free.

**These two should land together.** MAC-derived addressing alone would make the device harder to
reach; the DNS redirect alone leaves the collision risk in place. Neither is scheduled yet.

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
