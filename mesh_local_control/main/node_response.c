#include "node_response.h"
#include "string.h"
#include "esp_mac.h"
#include "inttypes.h"

#define SENSOR_RESPONSE_QUEUE_LENGTH 10

static const char *TAG = "node_response";

// Static queue to hold sensor responses.
static QueueHandle_t sensorResponseQueue = NULL;

void node_response_init(void) {
    if (!sensorResponseQueue) {
        sensorResponseQueue = xQueueCreate(SENSOR_RESPONSE_QUEUE_LENGTH, sizeof(sensor_response_t));
    }
}

void node_response_push(const uint8_t *src_mac, const sensor_record_t *data) {
    if (!sensorResponseQueue) return;
    sensor_response_t resp;
    memcpy(resp.mac, src_mac, sizeof(resp.mac));
    resp.sensor_data = *data;
    // Post without blocking.
    ESP_LOGW("PUSH", "Pushing response from " MACSTR, MAC2STR(resp.mac));
    xQueueSend(sensorResponseQueue, &resp, 0);
}

// Corrected version of waitForNodeResponse
bool waitForNodeResponse(const uint8_t *remote_mac, sensor_record_t *response, TickType_t timeout) {
    if (!sensorResponseQueue) return false;
    sensor_response_t recvResp;
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < timeout) {
        if (xQueueReceive(sensorResponseQueue, &recvResp, pdMS_TO_TICKS(10)) == pdPASS) {
            ESP_LOGI(TAG, "Received response from " MACSTR, MAC2STR(recvResp.mac));
            // Check if this is the expected sender.
            if (memcmp(recvResp.mac, remote_mac, sizeof(recvResp.mac)) == 0) {
                *response = recvResp.sensor_data;
                return true;
            }
        }
    }
    return false;
}
