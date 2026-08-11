#include "score_queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "score_q";

static score_item_t s_buf[SCORE_QUEUE_CAPACITY];
static int s_head = 0;   // index of oldest
static int s_count = 0;
static SemaphoreHandle_t s_mux = NULL;

void score_queue_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_head = 0;
    s_count = 0;
}

void score_queue_push(const score_item_t *item)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int tail = (s_head + s_count) % SCORE_QUEUE_CAPACITY;
    if (s_count == SCORE_QUEUE_CAPACITY) {
        ESP_LOGW(TAG, "queue full, dropping oldest id=%d", s_buf[s_head].id);
        s_buf[s_head] = *item;
        s_head = (s_head + 1) % SCORE_QUEUE_CAPACITY;
    } else {
        s_buf[tail] = *item;
        s_count++;
    }
    xSemaphoreGive(s_mux);
}

bool score_queue_peek(score_item_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_count > 0) {
        *out = s_buf[s_head];
        ok = true;
    }
    xSemaphoreGive(s_mux);
    return ok;
}

void score_queue_pop(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_count > 0) {
        s_head = (s_head + 1) % SCORE_QUEUE_CAPACITY;
        s_count--;
    }
    xSemaphoreGive(s_mux);
}

int score_queue_count(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int n = s_count;
    xSemaphoreGive(s_mux);
    return n;
}
