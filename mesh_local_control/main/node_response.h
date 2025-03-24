#ifndef NODE_RESPONSE_H
#define NODE_RESPONSE_H

#include "blockchain.h"  // for sensor_record_t
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t mac[6];
    sensor_record_t sensor_data;
} sensor_response_t;

// Must be called once at startup.
void node_response_init(void);

// Called by the receiver to push a new sensor response.
void node_response_push(const uint8_t *src_mac, const sensor_record_t *data);

// Waits for a sensor response from a given device within timeout (in ticks).
bool waitForNodeResponse(const uint8_t *remote_mac, sensor_record_t *response, TickType_t timeout);

#endif // NODE_RESPONSE_H
