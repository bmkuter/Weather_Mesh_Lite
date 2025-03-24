#include "node_response.h"
#include "string.h"

#define SENSOR_RESPONSE_QUEUE_LENGTH 10

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
    xQueueSend(sensorResponseQueue, &resp, 0);
}

bool waitForNodeResponse(const uint8_t *remote_mac, sensor_record_t *response, TickType_t timeout) {
    if (!sensorResponseQueue) return false;
    sensor_response_t recvResp;
    TickType_t start = xTaskGetTickCount();
    TickType_t remaining = timeout;
    while(remaining > 0) {
        if (xQueueReceive(sensorResponseQueue, &recvResp, remaining) == pdPASS) {
            // Compare the sender MAC address.
            if (memcmp(recvResp.mac, remote_mac, sizeof(recvResp.mac)) == 0) {
                *response = recvResp.sensor_data;
                return true;
            }
            // If not matching, adjust remaining wait time and continue.
        }
        remaining = timeout - (xTaskGetTickCount() - start);
    }
    return false;
}
