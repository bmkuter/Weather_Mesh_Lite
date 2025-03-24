#include "election_response.h"
#include <string.h>

typedef struct {
    uint8_t mac[6];
    uint16_t next_leader;
} election_message_t;

#define ELECTION_QUEUE_LENGTH 10
static QueueHandle_t electionQueue = NULL;

void election_response_push(const uint8_t *src_mac, uint16_t next_leader) {
    if (!electionQueue) return;
    election_message_t msg;
    memcpy(msg.mac, src_mac, sizeof(msg.mac));
    msg.next_leader = next_leader;
    xQueueSend(electionQueue, &msg, 0);
}

bool waitForElectionMessage(uint16_t *next_leader, TickType_t timeout) {
    if (!electionQueue) return false;
    election_message_t msg;
    if (xQueueReceive(electionQueue, &msg, timeout) == pdPASS) {
        *next_leader = msg.next_leader;
        return true;
    }
    return false;
}

void election_response_init(void) {
    if (!electionQueue) {
        electionQueue = xQueueCreate(ELECTION_QUEUE_LENGTH, sizeof(election_message_t));
    }
}
