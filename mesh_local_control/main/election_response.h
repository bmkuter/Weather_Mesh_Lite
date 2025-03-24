#ifndef ELECTION_RESPONSE_H
#define ELECTION_RESPONSE_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_mesh_lite.h"

#define ELECTION_QUEUE_LENGTH 10

// Now the election message holds a leader MAC address.
typedef struct {
    uint8_t leader_mac[ESP_NOW_ETH_ALEN];
} election_message_t;

// Updated prototypes: waitForElectionMessage now returns a MAC address.
bool waitForElectionMessage(uint8_t *leader_mac, TickType_t timeout);
void election_response_push(const uint8_t *src_mac, const uint8_t *leader_mac);
void election_response_init(void);

#endif // ELECTION_RESPONSE_H
