# OpenMANET XIAO Gateway — Release `v0.1.0-alpha`

> **Alpha Release Notice:** This is the initial alpha release of the OpenMANET XIAO Gateway firmware. It establishes the core dual-role bridging architecture, ATAK/CoT multicast routing, on-device cyberpunk HUD Web UI, serial CLI provisioning, and browser-based Web Flasher.

---

## 🛰️ Project Overview

The **OpenMANET XIAO Gateway** is a standalone, ultra-compact tactical communications gateway built on the **Seeed Studio XIAO ESP32-S3** paired with the **Seeed Studio XIAO WM6108 (Wi-Fi HaLow MM6108)** expansion board.

It bridges standard commercial off-the-shelf (COTS) smartphones and tablets running **ATAK (Android Team Awareness Kit)** or **iTAK** over standard 2.4 GHz Wi-Fi to a long-range, sub-GHz **Wi-Fi HaLow (IEEE 802.11ah, 902–928 MHz US)** mesh backhaul.

```
┌─────────────────────────┐          ┌──────────────────────────────────┐          ┌─────────────────────────┐
│       ATAK Phone        │  2.4GHz  │      OpenMANET XIAO Gateway      │  900MHz  │  Mesh Pi / Base Station │
│ (172.16.50.x on SoftAP) │ ◄──────► │   (ESP32-S3 + WM6108 HaLow)      │ ◄──────► │ (192.168.10.x Backhaul) │
│  UDP 239.2.3.1:6969     │  Wi-Fi   │ NVS Config · CoT Relay · Web HUD │  HaLow   │   ATAK / CoT Server     │
└─────────────────────────┘          └──────────────────────────────────┘          └─────────────────────────┘
```

---

## ⚡ Key Capabilities & Architecture

### 1. Dual Operational Roles
A single, unified firmware image supports two operational modes selectable at runtime:
* **Client Role (`GW_ROLE_CLIENT`, Default)**:
  * **Downlink**: 2.4 GHz SoftAP (`172.16.50.1/24`, DHCP server enabled) for operator phone/tablet connections.
  * **Uplink**: Sub-GHz Wi-Fi HaLow station associating to a central Mesh Pi or Relay gateway.
  * **Traffic**: Local clients receive routed access and NAT to the HaLow mesh backhaul.
* **Relay Role (`GW_ROLE_RELAY`)**:
  * **Uplink**: 2.4 GHz Wi-Fi Station associating to an upstream field router, starlink, or mesh Pi AP.
  * **Downlink**: Sub-GHz Wi-Fi HaLow Access Point (`172.16.60.1/24`) broadcasting a long-range 900 MHz network for leaf XIAO nodes.

### 2. ATAK Cursor-on-Target (CoT) Multicast Forwarding
* Dedicated bidirectional bridge for ATAK UDP multicast traffic (`239.2.3.1:6969` by default, configurable).
* Ingress and egress CoT packets are automatically ingested, validated, and forwarded across the 2.4 GHz Wi-Fi $\leftrightarrow$ 900 MHz HaLow network boundary with zero client-side reconfiguration.

### 3. Cyberpunk HUD Web UI (`http://172.16.50.1`)
An on-device, fully embedded responsive web application accessible offline over the local SoftAP:
* **Overview Dashboard**: Real-time link status, 4-bar visual RSSI meter (`[■■■□] -74 dBm`), connected client counter, heap diagnostics, and active dual-OTA partition slot.
* **Wireless & Radios**: Interactive role switcher cards, 2.4 GHz SoftAP configuration, and live HaLow airwaves scanner.
* **Two-Part Frequency Selector**: Clean, gated bandwidth selector (`8 MHz`, `4 MHz`, `2 MHz`, `1 MHz`) that dynamically limits channel choices strictly to legal US regulatory domain channels.
* **Services**: Callsign / Node ID identification and ATAK CoT multicast group/port settings.
* **Live Log Console**: In-RAM ring log reader with 3-second auto-refresh and one-click clipboard copy.
* **Docked HUD Action Bar**: Persistent bottom action bar for immediate configuration saving and reboot triggering.

### 4. Serial REPL & Provisioning CLI (`xiao-gw>`)
Accessible over the native USB-C port (`115200 8N1`):
* Interactive commands: `gwcfg-status`, `gwcfg-scan`, `gwcfg-set-role`, `gwcfg-radio`, `gwcfg-set-uplink`, `gwcfg-set-softap`, `gwcfg-set-cot`, `gwcfg-save`, `gwcfg-reset-defaults`, and `gwcfg-reboot`.
* **Hardware Recovery**: Holding the physical **BOOT button** on power-up executes a safe factory reset back to default client configuration.

### 5. Web Flasher & Release Automation
* **Browser-Based Flashing**: WebUSB-powered flasher using ESP Web Tools (Chrome/Edge compatible) requiring zero software installation.
* **Tag-Gated Release Pipeline**: Clear separation between official Tagged Releases (`v0.1.0-alpha`) and continuous Development Builds (`main`).
* **N-3 Rollback Retention**: GitHub Pages deployment automatically preserves the latest release plus up to 3 previous releases ($N-3$ rollback history).
* **Dual-OTA Flash Layout**: Partitioned for robust Over-The-Air firmware updates (`bootloader` at `0x0000`, `partition-table` at `0x8000`, `otadata` at `0x10000`, `app` at `0x20000`).

---

## 📋 Hardware Specification & Operating Limits

| Parameter | Specification | Notes |
|---|---|---|
| **MCU Board** | Seeed Studio XIAO ESP32-S3 | Dual-core Xtensa LX7 @ 240MHz, 8MB Flash |
| **HaLow Board** | Seeed Studio XIAO WM6108 | Quectel FGH100M-H (Morse Micro MM6108) |
| **HaLow Frequency** | 902.0 MHz – 928.0 MHz (US) | Compile-time fixed (`CONFIG_HALOW_COUNTRY_CODE="US"`) |
| **HaLow Bandwidths** | 1 MHz, 2 MHz, 4 MHz, 8 MHz | Configurable via Web UI and CLI |
| **Wi-Fi Local** | 2.4 GHz 802.11 b/g/n (ESP32-S3) | SoftAP mode (Default channel 6) |
| **Power Requirements** | 5V via USB-C or 5V VBUS pad | **Important:** The HaLow PA/LNA requires the 5V rail |
| **Antenna Requirements** | Separate 2.4 GHz & Sub-GHz antennas | **Never power on without sub-GHz antenna attached** |

---

## 🛠️ Flash Partition Map

```
Offset      Size      Partition Name       Content
─────────────────────────────────────────────────────────────────────────────
0x00000000  32 KB     bootloader           Second stage ESP-IDF bootloader
0x00008000   4 KB     partition_table      Custom dual-OTA partition table
0x00009000  16 KB     nvs                  NVS configuration storage (GWCF v4)
0x00010000   8 KB     otadata              Active OTA boot slot state
0x00020000  1.5 MB    ota_0 (app)          Primary firmware application
0x001A0000  1.5 MB    ota_1 (app)          Secondary OTA update slot
```

---

## 🚀 Getting Started

### 1. Flashing Firmware
1. Open the [OpenMANET Web Flasher](https://d3mocide.github.io/OpenMANET-XIAO-Gateway/) in Google Chrome or Microsoft Edge.
2. Connect your assembled XIAO ESP32-S3 + WM6108 node via USB-C.
3. Select **`[Latest Release] v0.1.0-alpha`** and click **Install OpenMANET Gateway**.

### 2. First-Time Configuration
1. Power up the device. Connect your phone or laptop to the default Wi-Fi network:
   * **SSID:** `XIAO-HaLow-GW`
   * **Password:** *(Open / None by default)*
2. In your browser, navigate to:
   ```
   http://172.16.50.1
   ```
3. Set your **Node Callsign**, select your **Node Role**, configure your **Mesh Uplink credentials** (or scan airwaves in the Wireless tab), set a secure **Wi-Fi Password**, and click **Save Config** $\rightarrow$ **Reboot Node**.
4. Open ATAK on your connected device; multicast CoT traffic is bridged automatically.
