#include "Arduino.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "arduino_shim";

void pinMode(uint8_t pin, uint8_t mode) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << pin);
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    if (mode == OUTPUT) {
        cfg.mode = GPIO_MODE_OUTPUT;
    } else if (mode == INPUT_PULLUP) {
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    } else {
        cfg.mode = GPIO_MODE_INPUT;
    }
    gpio_config(&cfg);
}

void digitalWrite(uint8_t pin, uint8_t val) {
    gpio_set_level((gpio_num_t)pin, val);
}

int digitalRead(uint8_t pin) {
    return gpio_get_level((gpio_num_t)pin);
}

unsigned long micros() {
    return (unsigned long)esp_timer_get_time();
}

unsigned long millis() {
    return (unsigned long)(esp_timer_get_time() / 1000);
}

void delay(unsigned long ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// Minimal Serial implementation -- routes to ESP_LOGI
HardwareSerial Serial;

void HardwareSerial::begin(unsigned long baud) {
    ESP_LOGI(TAG, "Serial.begin(%lu) -- using ESP-IDF logging", baud);
}

void HardwareSerial::println(const char *msg) {
    ESP_LOGI(TAG, "%s", msg);
}

void HardwareSerial::println(int val) {
    ESP_LOGI(TAG, "%d", val);
}
