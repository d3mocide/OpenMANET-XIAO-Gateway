#pragma once

/* Board-level GPIO assignments for the Seeed XIAO ESP32-S3 carrying the Seeed
 * XIAO WM6108 HaLow HAT.
 *
 * The HaLow HAT occupies GPIO 1, 2, 3, 4, 5, 7, 8 and 9 (RESET_N, WAKE, IRQ,
 * SPI CS, BUSY, SCK, MISO, MOSI - see the CONFIG_MM_* block in
 * sdkconfig.defaults, and design/HARDWARE.md for the full table). Nothing here
 * may overlap that set; both pins below are outside it.
 *
 * If a different HaLow HAT is used, the CONFIG_MM_* pins change and these two
 * must be re-checked against the new assignment before the firmware is built.
 */

/* The XIAO ESP32-S3's on-board user LED (the orange one next to the USB-C
 * connector). It is wired to 3V3 through the LED, so the GPIO sinks current:
 * driving the pin LOW turns the LED ON. Used by status_led.c as the only
 * bring-up indicator available when neither a serial cable nor a phone is
 * attached. */
#define BOARD_STATUS_LED_GPIO       21
#define BOARD_STATUS_LED_ACTIVE_LOW 1

/* The XIAO ESP32-S3's BOOT button, shared with the GPIO0 strapping pin. Held
 * LOW while pressed. Watched at runtime by factory_reset.c as the
 * no-cable-required config recovery path.
 *
 * Note this is explicitly *not* a "hold during power-on" check: holding GPIO0
 * low at reset is what puts the chip into the ROM serial bootloader, so the
 * app never runs to observe it. The reset is therefore a sustained hold while
 * the firmware is already running. */
#define BOARD_BOOT_BUTTON_GPIO      0
