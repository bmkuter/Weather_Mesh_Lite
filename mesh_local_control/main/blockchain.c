#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "temperature_probe.h"  // Provides temperature_probe_read_temperature() and ..._humidity()
#include "blockchain.h"
#include "consensus.h"
#include "mesh_networking.h"
#include "node_id.h"
#include "esp_log.h"

#define BLOCKCHAIN_BUFFER_SIZE 16  // Maximum blocks stored locally

static const char *TAG = "BLOCKCHAIN";
static block_t *blockchain_buffer = NULL;
static size_t blockchain_size = 0;
static SemaphoreHandle_t blockchain_mutex = NULL;

// Add a helper function to compute a dummy merkle root.
static void calculate_merkle_root(block_t *block) {
    // Simple dummy hash: use XOR of block_id and timestamp for every byte.
    uint8_t hash = (uint8_t)(block->block_id ^ block->timestamp);
    for (int i = 0; i < sizeof(block->merkle_root); i++) {
        block->merkle_root[i] = hash;
    }
}

uint32_t blockchain_init(void)
{
    blockchain_buffer = malloc(sizeof(block_t) * BLOCKCHAIN_BUFFER_SIZE);
    if (!blockchain_buffer) {
        ESP_LOGE(TAG, "Failed to allocate blockchain buffer");
        return 1;
    }
    blockchain_size = 0;
    blockchain_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Blockchain initialized");

    return 0;
}

bool blockchain_add_block(block_t *new_block)
{
    bool result = false;
    if (xSemaphoreTake(blockchain_mutex, portMAX_DELAY)) {
        if (blockchain_size < BLOCKCHAIN_BUFFER_SIZE) {
            memcpy(&blockchain_buffer[blockchain_size], new_block, sizeof(block_t));
            blockchain_size++;
            result = true;
        } else {
            ESP_LOGE(TAG, "Blockchain buffer full");
        }
        xSemaphoreGive(blockchain_mutex);
    }
    return result;
}

bool blockchain_get_last_block(block_t *block_out)
{
    bool result = false;
    if (xSemaphoreTake(blockchain_mutex, portMAX_DELAY)) {
        if (blockchain_size > 0) {
            memcpy(block_out, &blockchain_buffer[blockchain_size - 1], sizeof(block_t));
            result = true;
        }
        xSemaphoreGive(blockchain_mutex);
    }
    return result;
}

void blockchain_print_last_block(void)
{
    block_t last;
    if (blockchain_get_last_block(&last)) {
        ESP_LOGI(TAG, "----- Latest Block -----");
        ESP_LOGI(TAG, "Block ID: 0x%" PRIx32 ", Timestamp: 0x%" PRIx32, last.block_id, last.timestamp);
        ESP_LOGI(TAG, "PoP Proof: %s", last.pop_proof);
        for (int i = 0; i < MAX_NODES; i++) {
            sensor_record_t *record = &last.node_data[i];
            if (record->node_id != 0) {
                ESP_LOGI(TAG, "Node %d: Temp: %.2f°C, Humidity: %.2f%%",
                         record->node_id, record->temperature, record->humidity);
            }
        }
        ESP_LOGI(TAG, "------------------------");
    } else {
        ESP_LOGI(TAG, "No block available");
    }
}

// Modify blockchain_create_block to remove dummy merkle root assignment.
void blockchain_create_block(block_t *new_block, sensor_record_t sensor_data[MAX_NODES])
{
    memset(new_block, 0, sizeof(block_t));
    new_block->timestamp = (uint32_t)time(NULL);
    new_block->block_id = blockchain_size;  // Use current blockchain size as block ID

    block_t last;
    if (blockchain_get_last_block(&last)) {
        memcpy(new_block->prev_hash, last.merkle_root, sizeof(last.merkle_root));
    } else {
        memset(new_block->prev_hash, 0, sizeof(new_block->prev_hash));
    }
    
    for (int i = 0; i < MAX_NODES; i++) {
        new_block->node_data[i] = sensor_data[i];
    }
    // Fill dummy heatmap.
    for (int i = 0; i < HEATMAP_SIZE; i++) {
        new_block->heatmap[i] = i;
    }
    // Removed dummy merkle_root assignment.
    // The pop_proof field will be set later by consensus_generate_pop_proof().
}

void blockchain_receive_block(const uint8_t *data, uint16_t len)
{
    if (len != sizeof(block_t)) {
        ESP_LOGE(TAG, "Received block size mismatch: expected %d, got %d", (int)sizeof(block_t), len);
        return;
    }
    block_t incoming_block;
    memcpy(&incoming_block, data, len);
    if (blockchain_add_block(&incoming_block)) {
        ESP_LOGI(TAG, "Block 0x%" PRIx32 " received and added", incoming_block.block_id);
    } else {
        ESP_LOGE(TAG, "Failed to add received block 0x%" PRIx32, incoming_block.block_id);
    }
}

// New function: print entire blockchain history.
void blockchain_print_history(void)
{
    if (xSemaphoreTake(blockchain_mutex, portMAX_DELAY)) {
        ESP_LOGI(TAG, "===== Blockchain History =====");
        for (size_t i = 0; i < blockchain_size; i++) {
            block_t *current = &blockchain_buffer[i];
            ESP_LOGI(TAG, "Block %d: ID=0x%" PRIx32 ", Timestamp=0x%" PRIx32, i, current->block_id, current->timestamp);
            ESP_LOGI(TAG, "PoP Proof: %s", current->pop_proof);
            for (int j = 0; j < MAX_NODES; j++) {
                sensor_record_t *record = &current->node_data[j];
                if (record->node_id != 0) {
                    ESP_LOGI(TAG, "  Node %d: Temp: %.2f°C, Humidity: %.2f%%",
                             record->node_id, record->temperature, record->humidity);
                }
            }
        }
        ESP_LOGI(TAG, "===============================");
        xSemaphoreGive(blockchain_mutex);
    }
}

// In sensor_blockchain_task: update timestamp and calculate hash before Proof-of-participation.
void sensor_blockchain_task(void *pvParameters)
{
    uint32_t err_status = 0;
    err_status = blockchain_init();
    if (err_status != 0)
    {
        /* code */
        ESP_LOGE(TAG, "Failed to initialize blockchain");
        vTaskDelete(NULL);
    }
    
    ESP_LOGV(TAG, "Blockchain initialized");
    consensus_init(get_my_node_id());

    ESP_LOGI(TAG, "Starting sensor_blockchain_task");

    while (1) {
        // Get the current sensor reading.
        sensor_record_t my_sensor = {0};
        my_sensor.node_id = get_my_node_id();
        my_sensor.timestamp = (uint32_t)time(NULL);
        my_sensor.temperature = temperature_probe_read_temperature();
        my_sensor.humidity = temperature_probe_read_humidity();
        // (Optionally, you can also fill in neighbor RSSI values here.)

        // For this example, create a block that contains only this node’s sensor data.
        sensor_record_t sensor_data[MAX_NODES] = {0};
        sensor_data[0] = my_sensor;

        block_t new_block;
        blockchain_create_block(&new_block, sensor_data);
        // Ensure fresh timestamp from the task.
        new_block.timestamp = (uint32_t)time(NULL);
        // Calculate new hash for the block.
        calculate_merkle_root(&new_block);
        // Generate Proof-of-participation after hash calculation.
        consensus_generate_pop_proof(&new_block, get_my_node_id());

        // If this node is the leader, send the block.
        if (consensus_am_i_leader(new_block.block_id)) {
            uint32_t node_count = 0;
            // Get the list of nodes in the mesh.
            const node_info_list_t *list = esp_mesh_lite_get_nodes_list(&node_count);
            while (list) {
                esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE,
                                                    list->node->mac_addr,
                                                    (const uint8_t *)&new_block,
                                                    sizeof(new_block));
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send block to " MACSTR
                                     ", ret=0x%x:%s", MAC2STR(list->node->mac_addr), ret, esp_err_to_name(ret));
                }
                list = list->next;
            }
            ESP_LOGI(TAG, "Block %" PRIu32 " broadcast", new_block.block_id);
            ESP_LOGI(TAG, "Block Data: Temp: %.2f°C, Humidity: %.2f%%",
                     my_sensor.temperature, my_sensor.humidity);
            blockchain_add_block(&new_block);
        } else {
            ESP_LOGI(TAG, "Not leader for Block %" PRIu32 ", waiting for leader broadcast", new_block.block_id);
        }

        // Wait for 60 seconds before generating the next block.
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}