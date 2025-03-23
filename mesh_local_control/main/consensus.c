#include "consensus.h"
#include "mesh_networking.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <inttypes.h>  // For PRIu32
#include "esp_mesh_lite.h"  // New: for getting mesh node list

static const char *TAG = "CONSENSUS";
static uint16_t my_node_id = 0;
static uint16_t total_nodes = 3;  // For demonstration purposes.

void consensus_init(uint16_t node_id)
{
    my_node_id = node_id;
    total_nodes = 3;
    ESP_LOGI(TAG, "Consensus initialized for Node %d", my_node_id);
}

bool consensus_am_i_leader(uint32_t block_id)
{
    uint32_t nodes_count = 0;
    // Get the list of online nodes (excluding self)
    const node_info_list_t *node_list = esp_mesh_lite_get_nodes_list(&nodes_count);
    ESP_LOGI(TAG, "Node %" PRIu16 ": Received %" PRIu32 " online nodes", my_node_id, nodes_count);
    // Total online nodes = online list + self.
    uint32_t total_online = nodes_count + 1;
    ESP_LOGI(TAG, "Total online nodes: %" PRIu32, total_online);
    ESP_LOGI(TAG, "Node %" PRIu16 ": Checking if I am the leader for Block %" PRIu32, my_node_id, block_id);

    // Seed random generator with block_id and current time.
    srand(block_id + (uint32_t)time(NULL));
    uint32_t leader_index = rand() % total_online;

    bool result = (leader_index == 0);
    ESP_LOGI(TAG, "Block %" PRIu32 ": Elected Leader index %" PRIu32 " out of %" PRIu32 " nodes. Am I leader? %s",
             block_id, leader_index, total_online, result ? "Yes" : "No");
    return result;
}

void consensus_generate_pop_proof(block_t *block, uint16_t leader_node_id)
{
    uint32_t nonce = rand();
    // Use PRIu32 to print block->block_id and nonce.
    snprintf(block->pop_proof, sizeof(block->pop_proof), "Leader:%d;Block:%" PRIu32 ";Nonce:%" PRIu32,
             leader_node_id, block->block_id, nonce);
    ESP_LOGI(TAG, "Generated PoP proof for Block %" PRIu32 ": %s", block->block_id, block->pop_proof);
}

bool consensus_verify_block(block_t *block, sensor_record_t *my_sensor_data)
{
    uint16_t reported_leader = 0;
    char proof_copy[64];
    strncpy(proof_copy, block->pop_proof, sizeof(proof_copy));
    proof_copy[sizeof(proof_copy) - 1] = '\0';

    char *token = strtok(proof_copy, ";");
    while (token != NULL) {
        if (strncmp(token, "Leader:", 7) == 0) {
            sscanf(token + 7, "%hu", &reported_leader);
            break;
        }
        token = strtok(NULL, ";");
    }
    uint16_t expected_leader = (block->block_id % total_nodes) + 1;
    if (reported_leader != expected_leader) {
        ESP_LOGE(TAG, "Block %" PRIu32 " verification failed: expected leader %d but got %d",
                 block->block_id, expected_leader, reported_leader);
        return false;
    }
    // Check that our sensor data in the block matches our local measurement.
    for (int i = 0; i < MAX_NODES; i++) {
        sensor_record_t *record = &block->node_data[i];
        if (record->node_id == my_node_id) {
            if ((record->temperature != my_sensor_data->temperature) ||
                (record->humidity != my_sensor_data->humidity)) {
                ESP_LOGE(TAG, "Node %d: Block %" PRIu32 " sensor data mismatch!", 
                         my_node_id, block->block_id);
                return false;
            }
            break;
        }
    }
    return true;
}

void consensus_handle_dispute(uint32_t block_index, const uint8_t *src_mac)
{
    // Here we assume MAC2STR is defined in ESP-IDF headers.
    ESP_LOGE(TAG, "Dispute received for Block %" PRIu32 " from " MACSTR,
             block_index, MAC2STR(src_mac));
    // Further action (e.g., marking block invalid) would be taken here.
}
