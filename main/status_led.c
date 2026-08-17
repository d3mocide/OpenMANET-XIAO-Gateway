#include <stdbool.h>
#include <stdint.h>

#include "status_led.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "task_stats.h"
#include "uplink_halow.h"
#include "uplink_wifi.h"

static const char *TAG = "status_led";

static gw_node_role_t s_role = GW_ROLE_CLIENT;

/* One tick per pattern slot. 125ms gives a slow blink of 1Hz over an 8-slot
 * pattern while still being fast enough for a triple-blink to read as one. */
#define TICK_MS       125
#define PATTERN_SLOTS 8

static volatile bool s_attention = false;

/* Bit per slot, LSB first: 1 = LED on. Indexed by uplink_link_state_t via
 * designated initializers, so the mapping stays readable as states are added;
 * the task bounds-checks the index and falls back to the "searching" pattern
 * rather than reading past the end if a new state arrives without one. */
static const uint8_t s_patterns[] = {
    [UPLINK_LINK_RADIO_FAILED] = 0b00010101, /* fast triple-blink: nothing works */
    /* Single short flash per second: alive and waiting to be told what to
     * join. Deliberately the sparsest pattern here - it has to read as "idle,
     * needs you" rather than as any of the busy/failed patterns below. */
    [UPLINK_LINK_UNCONFIGURED] = 0b00000001,
    [UPLINK_LINK_DOWN]         = 0b00001111, /* slow 1Hz blink: searching */
    [UPLINK_LINK_ASSOCIATING]  = 0b00001111, /* same - it's still searching */
    [UPLINK_LINK_ASSOCIATED]   = 0b00000101, /* double-blink: no DHCP lease yet */
    [UPLINK_LINK_UP]           = 0b11111111, /* solid: usable */
};

/* Steady fast blink, distinct from every link-state pattern above. */
#define ATTENTION_PATTERN 0b01010101

/* GW_ROLE_RELAY has no HaLow-radio-init failure mode in the same sense as
 * GW_ROLE_CLIENT's uplink_halow (the native 2.4GHz radio doesn't have a
 * discrete SPI-bring-up step that can fail the way an external radio can -
 * see uplink_wifi.h), so this only ever maps into the DOWN/ASSOCIATING/
 * ASSOCIATED/UP/UNCONFIGURED slots of s_patterns[], never RADIO_FAILED.
 * Reflects uplink_wifi's state - the relay's link back to the Pi - rather
 * than the HaLow AP's own state, on the same reasoning the client role's LED
 * has always used: whether this node can actually reach the mesh is the
 * single most operationally important signal, more so than whether a leaf is
 * currently associated to it. */
static uplink_link_state_t relay_led_state(void)
{
    switch (uplink_wifi_get_link_state()) {
    case WIFI_UPLINK_UNCONFIGURED: return UPLINK_LINK_UNCONFIGURED;
    case WIFI_UPLINK_DOWN:         return UPLINK_LINK_DOWN;
    case WIFI_UPLINK_CONNECTING:   return UPLINK_LINK_ASSOCIATING;
    case WIFI_UPLINK_ASSOCIATED:   return UPLINK_LINK_ASSOCIATED;
    case WIFI_UPLINK_UP:           return UPLINK_LINK_UP;
    default:                       return UPLINK_LINK_DOWN;
    }
}

static void led_write(bool on)
{
#if BOARD_STATUS_LED_ACTIVE_LOW
    gpio_set_level(BOARD_STATUS_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(BOARD_STATUS_LED_GPIO, on ? 1 : 0);
#endif
}

static void status_led_task(void *arg)
{
    (void)arg;
    uint8_t slot = 0;

    for (;;) {
        uint8_t pattern;
        if (s_attention) {
            pattern = ATTENTION_PATTERN;
        } else {
            uplink_link_state_t state =
                (s_role == GW_ROLE_RELAY) ? relay_led_state() : uplink_halow_get_link_state();
            pattern = ((size_t)state < sizeof(s_patterns) / sizeof(s_patterns[0]))
                          ? s_patterns[state]
                          : s_patterns[UPLINK_LINK_DOWN];
        }

        led_write((pattern >> slot) & 1u);
        slot = (slot + 1) % PATTERN_SLOTS;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t status_led_start(gw_node_role_t role)
{
    s_role = role;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    led_write(false);

    /* Small stack: the task only reads an enum and toggles a pin. */
    if (xTaskCreate(status_led_task, "status_led", GW_STACK_STATUS_LED, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "status LED on GPIO%d", BOARD_STATUS_LED_GPIO);
    return ESP_OK;
}

void status_led_set_attention(bool on)
{
    s_attention = on;
}
