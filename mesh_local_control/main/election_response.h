#ifndef ELECTION_RESPONSE_H
#define ELECTION_RESPONSE_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

bool waitForElectionMessage(uint16_t *next_leader, TickType_t timeout);
void election_response_push(const uint8_t *src_mac, uint16_t next_leader);
void election_response_init(void);

#endif // ELECTION_RESPONSE_H
