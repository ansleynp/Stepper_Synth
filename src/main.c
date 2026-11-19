/*
 * Stepper Synth firmware STARTER only.
 * This is a basic PlatformIO/ESP-IDF build test, NOT the synthesizer logic.
 * It does not receive/transmit MIDI recordings or drive the motors yet.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "stepper_synth";

void app_main(void)
{
    ESP_LOGI(TAG, "Stepper Synth starter firmware booted.");
    ESP_LOGI(TAG, "Desktop recording protocol is not implemented yet.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
