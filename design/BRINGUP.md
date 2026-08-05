# Hardware bring-up runbook

A step-by-step procedure for the first time this firmware meets real hardware: what to run, what a
pass looks like, and what each specific failure means.

The build-order checklist in [`PROGRESS.md`](PROGRESS.md) says *what* has to be proven. This says
*how*, and — more usefully — how to tell two identical-looking failures apart.

- **Prerequisite:** the node is assembled per [`HARDWARE.md`](HARDWARE.md), both antennas fitted.
- **Prerequisite:** a Pi running its HaLow radio in AP mode ([`pi_side_reference.md`](pi_side_reference.md)).
- **Last updated:** 2026-08-05

## The instruments you have

Three, and knowing which to reach for saves most of the time:

| Instrument | Reach for it when | Needs |
|---|---|---|
| **Status LED** | You're looking at the node and want to know how far it got | Nothing |
| **Web UI** at `http://172.16.50.1/` | You want detail: state, RSSI, IPs, client count, logs, scan | A phone/laptop on the node's Wi-Fi |
| **Serial console** (`idf.py monitor`) | The SoftAP itself isn't working, or you want a scan table | USB-C cable |

### LED patterns

| Pattern | Meaning | Go to |
|---|---|---|
| Fast triple-blink, repeating | Radio never initialized | Step 1 |
| Slow blink (1 Hz) | Radio up, searching / not associated | Step 2 |
| Double-blink | Associated, but no DHCP lease | Step 3 |
| Solid on | Uplink up — associated and leased | Step 4 |

The LED reads the same link state the web UI and `gwcfg-status` report, so the three never
disagree.

---

## Step 0 — Confirm the Pi side first

Five minutes on the Pi, before touching the XIAO. Everything downstream assumes these answers.

```sh
uci show wireless          # SSID, security mode, country
iw dev                     # is the HaLow interface in AP mode?
iw list                    # supported channels / vif combinations
batctl if                  # mesh interfaces
```

Write down:

1. **SSID** of the Pi's HaLow AP.
2. **Security mode** — must be one of `open` / `owe` / `sae`. HaLow has no WPA2-PSK; if the Pi is
   configured for something that maps to neither of those three, that has to be resolved first.
3. **Country / regulatory domain.** This is the one that decides which firmware image you flash,
   and it cannot be changed afterwards without reflashing.
4. **Whether the Pi hands out DHCP leases** on that interface, and from what pool.

## Step 1 — Radio is alive

Flash the firmware for the region from step 0, using the web flasher or `idf.py flash`. Attach the
serial console.

```
xiao-gw> gwcfg-radio
```

**Pass:** BCF API version, BCF board description, firmware and morselib version numbers print.
This is also logged automatically at every boot, so it's in the log even if you didn't ask.

That output means host↔MM6108 SPI communication works — which rules out wiring, pin config, BCF
file and chip selection all at once. It is the single most valuable check in this document,
because every one of those failures otherwise presents identically as "it never associates".

**Fail — nothing prints, or the boot log shows `HaLow uplink init failed`:**

The firmware deliberately does *not* start its reconnect loop in this state; it says so once and
leaves the SoftAP, web UI and console running so you can work. The LED triple-blinks.

Check, in order:
1. The HaLow board is fully seated and the right way round.
2. `CONFIG_MM_*` in `sdkconfig.defaults` matches your actual HAT ([`HARDWARE.md`](HARDWARE.md)).
3. `CONFIG_MM_BCF_FILE` is the right calibration file for that HAT.

## Step 2 — The Pi's AP is visible

```
xiao-gw> gwcfg-scan
```

or press **Scan for HaLow APs** in the web UI.

**Pass:** the Pi's SSID appears, with an RSSI and a frequency. Note the RSSI — it's your baseline
for everything that follows. Clicking a result in the web UI fills the SSID field for you, which
also eliminates typos as a cause of later failures.

**Fail — no APs at all.** Two distinct causes that look identical:

- **The AP is down or out of range.** Check the Pi.
- **The AP is on a channel this firmware may not legally use.** The scan only covers the channel
  list for this build's `CONFIG_HALOW_COUNTRY_CODE`, which both `gwcfg-scan` and the web UI print
  alongside the (empty) result for exactly this reason. If it doesn't match what you wrote down in
  step 0, reflash with the right region build before concluding anything.

Antenna check: if the scan finds the AP only when you're standing next to the Pi, suspect the
sub-GHz antenna before suspecting the radio.

## Step 3 — Associate, then get a lease

Set the uplink credentials — web UI, or:

```
xiao-gw> gwcfg-set-uplink <ssid> <psk|-> <open|owe|sae>
xiao-gw> gwcfg-save
```

Reboot. Then watch `gwcfg-status`, the web UI status panel, or the LED.

These are **two separate milestones** and the firmware reports them separately. That distinction is
the point of this step:

| State shown | Meaning | Cause to chase |
|---|---|---|
| `searching` | Not associated | SSID, security mode, region, or RF |
| `associating` | Association in progress | Transient — if it sticks here, credentials |
| `associated, no lease` | 802.11 association succeeded, DHCP did not | **Pi-side DHCP**, not the radio |
| `up` | Associated and leased | Move to step 4 |

A single "connected / not connected" flag would collapse rows 1 and 3 into one indistinguishable
failure. They have nothing to do with each other: row 1 is a radio/credentials problem on the XIAO,
row 3 is a DHCP problem on the Pi.

If it sits at `associated, no lease`, the firmware will restart its DHCP client once, then
disconnect and re-associate. Check the Pi is actually serving that interface.

## Step 4 — Local Wi-Fi and client connectivity

This step can be done at any time — it doesn't depend on the uplink — and it's the natural first
hardware test.

1. Join the node's Wi-Fi (`xiao-gateway` / `openmanet` by default).
2. You should get a `172.16.50.x` address.
3. Browse to `http://172.16.50.1/`.

Then, once the uplink is `up`:

4. **Ping something on the mesh by IP.** Proves NAT and routing.
5. **Resolve a hostname.** Proves the DNS server the node hands out in its DHCP leases works.

> **If you joined the node's Wi-Fi *before* the uplink came up**, disconnect and rejoin before
> testing DNS. The DNS server is copied from the uplink's own DHCP lease and pushed into the
> SoftAP's DHCP server at the moment the uplink comes up — clients already holding a lease keep
> their old, DNS-less one until they renew. This is normal and only affects the first boot of a
> session.

If step 4 works and step 5 doesn't, the log will say why: look for
`uplink DHCP lease carried no DNS server`, which means the Pi didn't offer one.

## Step 4a — Multicast over HaLow, in isolation

**Do this before involving the CoT relay.** Multicast over mesh routing is a classic silent-drop
point, and it fails in exactly the same way a broken relay does — so test them separately or you
will not be able to tell which one is at fault.

From a host on the mesh, send to `239.2.3.1:6969`, and confirm with a plain multicast receiver on
the Pi that the group traffic crosses the mesh at all. Only once that's proven should the relay be
in the picture.

## Step 5 — CoT relay

With the uplink `up`, the web UI's **CoT relay** stat should show the group and port rather than
`Waiting`. Then:

1. ATAK on a phone behind the node should see CoT from the mesh.
2. ATAK elsewhere on the mesh should see the phone.

If the relay says it started and nothing flows, step 4a is what tells you whether to look at the
relay or at the mesh.

## When something goes wrong and nobody was watching

Two things survive the event:

- **`GET /api/log`** (the web UI's *Device log* panel) — the last few KB of log output, held in
  RAM. This is what the serial console would have shown, available without a cable.
- **Core dumps** — a panic writes a backtrace to the flash coredump partition. Read it back later
  with `idf.py coredump-info`.

## Recovery

**Config wrong and the SoftAP unreachable?** Hold the **BOOT button for 5 seconds** while the node
is running. The LED switches to a fast blink after ~1.5 s to acknowledge; release before 5 s to
cancel. At 5 s the config is restored to defaults and the node reboots.

Note this is a *runtime* hold, not a hold-during-power-on: holding BOOT at reset puts the chip into
the ROM bootloader instead of running the firmware.

**Everything else:** the USB console is always there, and `gwcfg-reset` + `gwcfg-save` does the
same job.

## Recording results

Update the checklist in [`PROGRESS.md`](PROGRESS.md) as steps pass, and add anything learned about
the Pi to [`pi_side_reference.md`](pi_side_reference.md) — those open items are the ones blocking
other people from repeating this.
