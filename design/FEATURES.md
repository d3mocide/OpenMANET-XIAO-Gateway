# Feature implementation plan

The features this firmware does **not** have yet, what each one actually involves, and the order
they should land in.

This is the forward-looking companion to [`PROGRESS.md`](PROGRESS.md) (what exists today) and
[`TECHNICAL_REVIEW.md`](TECHNICAL_REVIEW.md) (what was found and fixed). Decisions already settled
are recorded here so the next pass doesn't relitigate them.

- **Last updated:** 2026-08-05

## Ordering, and why

```
   1. Web UI authentication  ──┐
                               ├──> 3. OTA update delivery
   2. OTA partition layout ────┘        (done: layout. blocked: delivery)

   4. Self-beacon (CoT)        independent, needs a working uplink to test
   5. Captive portal / mDNS    independent, pure UX
   6. Multi-group CoT relay    independent, small
   7. Power management         independent, needs real battery hardware
```

Only one hard dependency exists in this list, and it is a security one: **authentication must land
before any firmware-upload path exists.** Everything else can be done in any order, and should be
ordered by whatever hardware testing turns out to demand.

Nothing here should start before the bring-up checklist in [`BRINGUP.md`](BRINGUP.md) passes.
Features built against an unproven link are features debugged twice.

---

## 1. Web UI authentication

**Status:** designed in detail, not built. **Blocks:** shipping, and OTA delivery outright.

Full design and the reasoning behind each decision is in
[`TECHNICAL_REVIEW.md`](TECHNICAL_REVIEW.md) → "Deferred: web UI authentication". Summary of what
was already decided, so it isn't reopened:

| Decision | Choice | Why |
|---|---|---|
| Default password | **Forced change on first use** | A fixed default is non-compliant (CA SB-327, UK PSTI) and this board has no per-unit labelling step to communicate a random one. |
| TLS | **No** | No CA issues certificates for a private IP; a self-signed cert trains users to click through warnings, which is worse than none. WPA2 on the SoftAP is the transport protection. |
| Password on the wire | **Challenge-response** (nonce + HMAC) | With WPA2-PSK, anyone who knows the AP passphrase can decrypt other clients' traffic — and a team may share the Wi-Fi password without every member being an admin. |
| Crypto in the browser | **Bundled SHA-256/HMAC, ~2KB** | `crypto.subtle` only exists in secure contexts, and `http://172.16.50.1` is not one. It will be `undefined` on the device. **Do not "simplify" this back to WebCrypto later.** |
| Storage | PBKDF2-HMAC-SHA256, per-device random salt in NVS | mbedtls is already linked. Never store the password. |
| Sessions | `esp_random()` tokens, RAM only, idle timeout, `HttpOnly` + `SameSite=Strict` | Tokens should not survive a reboot. |
| Brute force | Lockout/backoff | The attacker is already on the LAN. |
| Recovery | `gwcfg-reset-auth` on the serial console | Physical presence is the right trust model. **An undocumented recovery path is the same as none.** |

**Implementation notes now that the surrounding code exists:**

- The existing `reject_if_remote()` gate stays. Authentication is *added to* the SoftAP-subnet
  check, not a replacement for it — subnet authorization and authentication defend against
  different attackers.
- `gw_config_t` gains a salt + hash + a "password has been set" flag. That is a layout change:
  bump `GW_CONFIG_VERSION` (see `gw_config.h`).
- The BOOT-button factory reset in `factory_reset.c` already restores defaults; it must clear the
  stored credential too, otherwise a forgotten password survives the one recovery path a
  field-deployed node has.

## 2. OTA partition layout

**Status: done.** Dual 3MB `ota_0`/`ota_1` slots plus `otadata`, on 8MB flash. Recorded here only
because it's half of item 3 and the two are easy to confuse.

## 3. OTA update delivery

**Status:** deferred by choice, not effort. **Blocked on item 1.**

An OTA endpoint means anyone who can reach the web UI can replace the firmware. Promoting "can
change my config" to "can replace my firmware" on an unauthenticated endpoint is an escalation, not
an increment.

In order:

1. Authentication (item 1). Nothing else ships first.
2. An update path — `esp_https_ota` from a known URL, or an authenticated upload endpoint.
3. `esp_ota_mark_app_valid_cancel_rollback()` in the app, **then** enable
   `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Turning rollback on before that call exists means every
   boot reverts.
4. A real end-to-end test on hardware, **including a deliberately bad image**, before it is relied
   on in the field.

The status API already reports the running partition (`system.partition`) — meaningless today,
since it is always `ota_0`, and the first thing you want to see once this exists.

## 4. Self-beacon — the node's own CoT

**Status:** the send primitive exists; nothing calls it. **Needs:** a working uplink to test.

`cot_relay_inject()` is already written as the generic send primitive `DESIGN.md` §5.5 asks for,
and the relay's own loop-prevention (source-address drop) already covers injected traffic. What's
missing is a producer.

Without this the gateway is **invisible on other people's ATAK screens** — it relays everyone
else's situational awareness while contributing none of its own.

**Scope decision to make:** the design assumes GPS, mirroring what Pi nodes do via gpsd. There is
no GPS on this hardware today. Two options, and they aren't exclusive:

- **Static/configured position.** A lat/lon in `gw_config_t`, set once when the node is placed.
  Cheap, no hardware, and genuinely useful for a node on a fixed mast — which is a large fraction
  of what these are for.
- **Real GPS.** A UART GPS module on the free pins. More useful for a mobile node, but it is new
  hardware, new config, and a new failure mode.

Either way the beacon should also carry what only this node knows: uplink state, RSSI, client
count, uptime, and battery once item 7 exists. That data already exists behind `/api/status`.

**Cost note:** a CoT beacon is an XML document. Generating it means either string-building
(cheap, fiddly) or pulling in a formatter. Budget a few KB either way; the app partition has ~45%
free.

## 5. Captive portal and mDNS

**Status:** not started. Pure UX, no protocol risk.

Today a user must *know* to browse to `172.16.50.1`. Two independent improvements:

- **mDNS** (`xiao-gw.local`). Small: ESP-IDF ships an `mdns` component. Works well on iOS/macOS,
  unevenly on Android.
- **Captive portal DNS.** A DNS responder on the SoftAP that answers every query with the node's
  own address, so joining the Wi-Fi pops the config page automatically. This is the one that
  actually solves the problem, on every platform. It is also the standard ESP-IDF pattern and
  interacts with nothing else here.

Note the interaction with item 1: a captive portal that lands on a login screen is a much better
first-run experience than one that lands on an open config form. Worth doing after auth, not
before.

## 6. Multi-group CoT relay

**Status:** not started. Small.

The relay handles exactly one group/port (`239.2.3.1:6969` by default). Some ATAK deployments use
additional groups. `gw_cot_config_t` would become a short array, `cot_relay_start()` would join
each, and the relay's arrival-interface logic is unchanged. Bump `GW_CONFIG_VERSION`.

Worth doing only if a real deployment needs it — speculative generality here costs config
complexity for every user.

## 7. Power management

**Status:** nothing at all. **Needs:** real battery hardware to measure against.

`DESIGN.md` opens by calling this "a battery/portable-power node", and the firmware currently does
nothing about it: no light sleep, no duty cycling, no battery voltage sensing, no low-power
reporting.

Do not guess at this before hardware. The things worth measuring first:

- Idle draw with the HaLow radio associated but passing no traffic.
- Draw while the SoftAP has clients.
- Whether the MM6108's own power-save modes (the SDK exposes them) are usable given the CoT relay
  needs to receive multicast promptly.

Then, and only then: battery voltage on an ADC pin, reported through `/api/status` and the
self-beacon, and a considered decision about sleep. Pre-tuning against a link whose real
characteristics are unknown is guessing — the same argument
[`TECHNICAL_REVIEW.md`](TECHNICAL_REVIEW.md) makes about throughput.

---

## Known limitations that are decisions, not gaps

These are recorded so they don't get "fixed" by accident:

- **No inbound unicast to a specific client.** NAT-only for v1 (`DESIGN.md` §4.3). batman-adv is
  L2 and won't route to a subnet behind a NAT'd leaf. The v2 fix, if needed, is a static route on
  the gateway Pi — not a firmware change.
- **NAT/relay initialize against the first uplink IP only.** If a later reconnect leases a
  *different* address they are not re-initialized. Whether this matters depends on whether lease
  changes actually happen, which hardware testing will answer. (The failure-retry path *is* fixed:
  a failed init now retries on the next reconnect.)
- **Region is build-time.** `CONFIG_HALOW_COUNTRY_CODE` cannot be made runtime-configurable — the
  SDK reads it from Kconfig before the radio scans. The web flasher's region picker is the
  mechanism, and that is by design.
