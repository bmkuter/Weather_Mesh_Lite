#include "consensus.h"
#include "mesh_networking.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <inttypes.h>
#include "esp_mesh_lite.h"
#include "election_response.h"

static const char *TAG = "CONSENSUS";
// Removed my_node_id; instead store the local MAC.
static uint8_t my_mac[ESP_NOW_ETH_ALEN] = {0};

void consensus_init(void)
{
    esp_wifi_get_mac(ESP_IF_WIFI_STA, my_mac);
    ESP_LOGI(TAG, "Consensus initialized for device " MACSTR, MAC2STR(my_mac));
}

bool consensus_am_i_leader(const uint8_t *leader_mac)
{
    uint8_t local_mac[ESP_NOW_ETH_ALEN] = {0};
    esp_wifi_get_mac(ESP_IF_WIFI_STA, local_mac);
    return (memcmp(leader_mac, local_mac, ESP_NOW_ETH_ALEN) == 0);
}

void consensus_generate_pop_proof(block_t *block, const uint8_t *leader_mac)
{
    uint32_t nonce = rand();
    // Now using timestamp instead of block_id.
    snprintf(block->pop_proof, sizeof(block->pop_proof),
             "Leader:" MACSTR ";Time:%" PRIu32 ";Nonce:%" PRIu32,
             MAC2STR(leader_mac), block->timestamp, nonce);
    ESP_LOGI(TAG, "Generated PoP proof for block (Time: %" PRIu32 "): %s", block->timestamp, block->pop_proof);
}

bool consensus_verify_block(block_t *block, sensor_record_t *my_sensor_data)
{
    uint8_t reported_leader[ESP_NOW_ETH_ALEN] = {0};
    char proof_copy[64];
    strncpy(proof_copy, block->pop_proof, sizeof(proof_copy));
    proof_copy[sizeof(proof_copy) - 1] = '\0';

    // Parse the "Leader:" field.
    if (strncmp(proof_copy, "Leader:", 7) == 0) {
        if (sscanf(proof_copy + 7,
                   "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                   &reported_leader[0], &reported_leader[1], &reported_leader[2],
                   &reported_leader[3], &reported_leader[4], &reported_leader[5]) != ESP_NOW_ETH_ALEN) {
            ESP_LOGE(TAG, "Failed to parse leader MAC from PoP proof");
            return false;
        }
    }

    // Verify our sensor data present in the block.
    for (int i = 0; i < MAX_NODES; i++) {
        sensor_record_t *record = &block->node_data[i];
        // Compare sensor record MAC with our local MAC.
        if (memcmp(record->mac, my_mac, ESP_NOW_ETH_ALEN) == 0) {
            if ((record->temperature != my_sensor_data->temperature) ||
                (record->humidity != my_sensor_data->humidity)) {
                ESP_LOGE(TAG, "Sensor data mismatch for device " MACSTR, MAC2STR(my_mac));
                return false;
            }
            break;
        }
    }
    return true;
}

void consensus_handle_dispute(uint32_t block_index, const uint8_t *src_mac)
{
    ESP_LOGE(TAG, "Dispute received for block (Time index): %" PRIu32 " from " MACSTR,
             block_index, MAC2STR(src_mac));
    // Further action (e.g., marking block invalid) would be taken here.
}
