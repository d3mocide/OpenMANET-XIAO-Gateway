#include <stdbool.h>

#include "factory_reset.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "provisioning.h"
#include "status_led.h"

static const char *TAG = "factory_reset";

#define POLL_INTERVAL_MS 100

/* Long enough that it can't be hit by accident (the BOOT button is also used
 * for flashing, so it gets pressed for other reasons), short enough to be
 * practical while standing over the node. */
#define HOLD_TO_RESET_MS 5000

/* The LED switches to its attention pattern this far into the hold, so the
 * operator knows the node saw the press and can let go to abort. */
#define HOLD_ACK_MS 1500

static gw_config_t *s_cfg = NULL;

static bool button_pressed(void)
{
    /* Pulled up externally; the button shorts it to ground. */
    return gpio_get_level(BOARD_BOOT_BUTTON_GPIO) == 0;
}

static void do_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset requested - restoring default config");

    provisioning_config_lock();
    provisioning_get_defaults(s_cfg);
    esp_err_t err = provisioning_save(s_cfg);
    provisioning_config_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "couldn't persist defaults: %s", esp_err_to_name(err));
        /* Rebooting anyway would come back up on the old stored config and
         * look like the button did nothing, so stay up and keep the LED
         * signalling instead - the operator still has the console. */
        return;
    }

    ESP_LOGW(TAG, "defaults saved, rebooting");
    /* Give the log line a moment to drain over USB before the CPU resets. */
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static void factory_reset_task(void *arg)
{
    (void)arg;
    uint32_t held_ms = 0;
    bool acked = false;

    for (;;) {
        if (button_pressed()) {
            held_ms += POLL_INTERVAL_MS;

            if (!acked && held_ms >= HOLD_ACK_MS) {
                acked = true;
                status_led_set_attention(true);
                ESP_LOGW(TAG, "hold for %u more ms to factory reset (release to cancel)",
                         (unsigned)(HOLD_TO_RESET_MS - held_ms));
            }

            if (held_ms >= HOLD_TO_RESET_MS) {
                do_factory_reset();
                /* Only reached if the save failed - stop counting so the
                 * warning isn't repeated every 100ms. */
                status_led_set_attention(false);
                held_ms = 0;
                acked = false;
            }
        } else {
            if (acked) {
                ESP_LOGI(TAG, "factory reset cancelled");
                status_led_set_attention(false);
            }
            held_ms = 0;
            acked = false;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t factory_reset_start(gw_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* The XIAO has an external pull-up on BOOT, but enabling the internal
         * one too costs nothing and keeps the pin defined if this is ever
         * moved to a board that doesn't. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(factory_reset_task, "factory_reset", 3072, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "hold BOOT (GPIO%d) for %u s to restore default config",
             BOARD_BOOT_BUTTON_GPIO, (unsigned)(HOLD_TO_RESET_MS / 1000));
    return ESP_OK;
}
