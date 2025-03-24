#include "election_response.h"
#include <string.h>

static QueueHandle_t electionQueue = NULL;

void election_response_push(const uint8_t *src_mac, const uint8_t *leader_mac) {
    if (!electionQueue) return;
    election_message_t msg;
    memcpy(msg.leader_mac, leader_mac, sizeof(msg.leader_mac));
    xQueueSend(electionQueue, &msg, 0);
}

bool waitForElectionMessage(uint8_t *leader_mac, TickType_t timeout) {
    if (!electionQueue) return false;
    election_message_t msg;
    if (xQueueReceive(electionQueue, &msg, timeout) == pdPASS) {
        memcpy(leader_mac, msg.leader_mac, sizeof(msg.leader_mac));
        return true;
    }
    return false;
}

void election_response_init(void) {
    if (!electionQueue) {
        electionQueue = xQueueCreate(ELECTION_QUEUE_LENGTH, sizeof(election_message_t));
    }
}
