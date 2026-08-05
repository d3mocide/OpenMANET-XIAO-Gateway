# Working notes for agents

ESP-IDF firmware for a Seeed XIAO ESP32-S3 + Seeed XIAO WM6108 (HaLow) node: phones associate to
the XIAO's 2.4 GHz SoftAP, and it relays their traffic — including ATAK CoT multicast — over a
HaLow uplink into an OpenMANET mesh via a Raspberry Pi.

Read [`design/ROADMAP.md`](design/ROADMAP.md) first. It has current status, the build-order
checklist, what isn't built yet, and the settled decisions.

## The rule that matters most

**Verify API semantics against upstream source, not memory.**

A compiling ESP-IDF build proves internal consistency. It does not prove an API means what the call
site assumes. Three separate review passes have each found bugs in clean-compiling, clean-warning
firmware that would have failed silently on hardware and looked like radio problems:

| Bug | What "obviously" seemed true | What the source said |
|---|---|---|
| CoT relay dropped 100% of datagrams | `IP_PKTINFO.ipi_addr` identifies the arrival interface | lwIP fills it from the packet's *destination* — always the multicast group. `ipi_ifindex` is the arrival signal. |
| SoftAP clients had no working DNS | The DHCP server offers a sensible default | `dhcpserver.c` initializes `dhcps_dns = 0x00` and only emits the option when explicitly enabled. |
| NAPT on the wrong netif | NAT belongs on the upstream interface | `esp_netif` sets the flag on exactly the netif you hand it and clears every other; it belongs on the AP side. |

The same discipline has also *saved* work — a note claiming `mmhalow_disconnect()` didn't exist was
wrong; fetching the component and reading `mmhalow.h` settled it in minutes. And `esp_http_server`
has no `HTTPD_503`/`HTTPD_409` members, which would have been compile errors.

So: when touching an unfamiliar API, fetch and read it.

```sh
# ESP-IDF at the pinned version
curl -s https://raw.githubusercontent.com/espressif/esp-idf/v5.5.1/<path>

# The HaLow component (headers, examples, board configs)
curl -s https://components.espressif.com/api/components/morsemicro/halow   # -> .versions[0].url
```

Cite the file and line in the code comment or commit message so the next person can re-check rather
than trust.

## Building and verifying

Requires ESP-IDF **v5.4.4+** (`morsemicro/halow`'s own floor); v5.5.1 is what's verified.

```sh
idf.py set-target esp32s3
idf.py build
```

- **Always run a real build before claiming a change works.** It's the only way to catch a
  compile/config error before it costs hardware-debugging time.
- **Never pipe `idf.py build` through `tail`/`head`** to shorten output — the shell exit code then
  reflects the pipe, not the build, and it will claim success on real failures. Grep the log for
  `error:`/`FAILED` instead of trusting `$?`.
- CI builds every push to `main` (all 9 regions) and every PR (US only), in the official
  `espressif/idf` Docker image.

Current baseline: zero errors, zero warnings, binary ~1.67 MB, 44% free in the 3 MB app slot.

## Things that look like cleanup but aren't

Each of these has a reason recorded in `design/ROADMAP.md` under "Settled decisions". Don't undo
them without reading it.

- **The auth design needs a bundled ~2 KB SHA-256/HMAC in `web_ui.html`.** `crypto.subtle` is only
  available in secure contexts and `http://172.16.50.1` is not one — switching to WebCrypto gives
  you `undefined` on the device, silently.
- **No hardcoded default admin password**, ever. Forced change on first use. This is a compliance
  constraint (CA SB-327, UK PSTI), not a preference.
- **Authentication lands before any firmware-upload endpoint**, and
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` stays off until the app calls
  `esp_ota_mark_app_valid_cancel_rollback()`.
- **NAPT goes on the SoftAP netif, the uplink is the default route, and the uplink's DNS is copied
  into the SoftAP's DHCP offers.** All three, or clients get an address and no connectivity.
- **`reject_if_remote()` stays** when auth arrives. Subnet authorization and authentication defend
  against different attackers.
- **Integer arithmetic for frequency formatting**, not `%f`. `CONFIG_LIBC_NEWLIB_NANO_FORMAT` is off
  today but is exactly the knob someone reaches for to shrink a binary, and the failure mode is
  garbage in the scan table during bring-up.
- **`CONFIG_HALOW_COUNTRY_CODE` cannot be made runtime-configurable.** The SDK reads it from Kconfig
  before the radio scans. The web flasher's region picker is the mechanism.

## Conventions

- **Comments explain *why*, and cite sources for non-obvious API behaviour.** The existing code does
  this heavily — match it. A comment that restates the code is noise; one that says which upstream
  file proves the call is correct is what stops the next person reverting a subtle fix.
- **Bump `GW_CONFIG_VERSION`** (`main/gw_config.h`) whenever `gw_config_t`'s layout *or the meaning
  of a field* changes. Stored config is discarded on mismatch, which is deliberate.
- **New GPIO goes in `main/board.h`** and must not collide with the `CONFIG_MM_*` pins (1, 2, 3, 4,
  5, 7, 8, 9).
- **Never log a passphrase.** `/api/log` serves the log ring over HTTP, and `GET /api/config` never
  echoes passwords back. Keep both true.
- **Update `design/ROADMAP.md`** when a checklist step passes or a feature lands. Keep it
  present-tense — historical narrative belongs in git history, not the doc.
- **Update `design/PI_SIDE.md`** whenever something about the Pi gets confirmed, and cite how.

## Layout

```
main/           firmware (see README.md for the per-file table)
design/         ROADMAP.md, HARDWARE.md, PI_SIDE.md — three docs, no more
docs/           the ESP Web Tools browser flasher page (deployed to GitHub Pages)
country-configs/ per-region CONFIG_HALOW_COUNTRY_CODE overrides for the CI matrix
```

`design/` deliberately holds exactly three documents. Content that doesn't fit one of them probably
belongs in a code comment or a commit message.
