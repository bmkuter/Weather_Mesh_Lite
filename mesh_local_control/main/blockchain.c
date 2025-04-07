#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "temperature_probe.h"
#include "blockchain.h"
#include "consensus.h"
#include "mesh_networking.h"
#include "node_id.h"
#include "esp_log.h"
#include "node_response.h"
#include "election_response.h"
#include "esp_mesh_lite.h"
#include "mbedtls/sha256.h"
#include "command_set.h"

#define BLOCKCHAIN_BUFFER_SIZE 16  // Maximum blocks stored locally

static const char *TAG = "BLOCKCHAIN";
static block_t *blockchain_head = NULL;    // Head of the linked list
static uint32_t blockchain_count = 0;          // Number of blocks in the blockchain
static SemaphoreHandle_t blockchain_mutex = NULL;

/**
 * Compute the SHA‑256 hash for the given block.
 * (Temporarily zero out the hash field so it is not included in the hash calculation).
 */
void calculate_block_hash(block_t *block) {
    // New fixed fields: block_num, timestamp, prev_hash, pop_proof, heatmap, and num_sensor_readings.
    size_t fixed_size = sizeof(block->block_num) + sizeof(block->timestamp) +
                        sizeof(block->prev_hash) + sizeof(block->pop_proof) +
                        sizeof(block->heatmap) + sizeof(block->num_sensor_readings);
    // Total size = fixed fields + all sensor records.
    size_t total_size = fixed_size + (block->num_sensor_readings * sensor_size);
    
    uint8_t *serial_buffer = malloc(total_size);
    if (!serial_buffer) {
        ESP_LOGE(TAG, "Failed to allocate serial buffer");
        return;
    }
    size_t offset = 0;
    
    // Serialize the new block member.
    memcpy(serial_buffer + offset, &block->block_num, sizeof(block->block_num));
    offset += sizeof(block->block_num);
    memcpy(serial_buffer + offset, &block->timestamp, sizeof(block->timestamp));
    offset += sizeof(block->timestamp);
    memcpy(serial_buffer + offset, block->prev_hash, sizeof(block->prev_hash));
    offset += sizeof(block->prev_hash);
    memcpy(serial_buffer + offset, block->pop_proof, sizeof(block->pop_proof));
    offset += sizeof(block->pop_proof);
    memcpy(serial_buffer + offset, block->heatmap, sizeof(block->heatmap));
    offset += sizeof(block->heatmap);
    memcpy(serial_buffer + offset, &block->num_sensor_readings, sizeof(block->num_sensor_readings));
    offset += sizeof(block->num_sensor_readings);
    
    // Serialize each sensor record from the linked list.
    sensor_record_t *cur = block->node_data;
    while (cur) {
        memcpy(serial_buffer + offset, cur->mac, sizeof(cur->mac));
        offset += sizeof(cur->mac);
        memcpy(serial_buffer + offset, &cur->timestamp, sizeof(cur->timestamp));
        offset += sizeof(cur->timestamp);
        memcpy(serial_buffer + offset, &cur->temperature, sizeof(cur->temperature));
        offset += sizeof(cur->temperature);
        memcpy(serial_buffer + offset, &cur->humidity, sizeof(cur->humidity));
        offset += sizeof(cur->humidity);
        memcpy(serial_buffer + offset, cur->rssi, MAX_NEIGHBORS * sizeof(int8_t));
        offset += MAX_NEIGHBORS * sizeof(int8_t);
        
        cur = cur->next;
    }
    
    ESP_LOGI(TAG, "Block serialized for hash calculation, total bytes: %d", total_size);
    // ESP_LOG_BUFFER_HEX_LEVEL(TAG, serial_buffer, total_size, ESP_LOG_INFO);
    
    uint8_t computed_hash[32] = {0};
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        ESP_LOGE(TAG, "SHA256 starts failed");
        free(serial_buffer);
        mbedtls_sha256_free(&ctx);
        return;
    }
    mbedtls_sha256_update(&ctx, serial_buffer, total_size);
    mbedtls_sha256_finish(&ctx, computed_hash);
    mbedtls_sha256_free(&ctx);
    
    free(serial_buffer);
    memcpy(block->hash, computed_hash, 32);
    ESP_LOGI(TAG, "Block hash computed");
}

size_t blockchain_serialize_block(const block_t *block, uint8_t **out_buffer) {
    ESP_LOGW(TAG, "Serializing block for transmission");
    size_t header_size = sizeof(block->block_num) + sizeof(block->timestamp) +
                         sizeof(block->prev_hash) + sizeof(block->hash) +
                         sizeof(block->pop_proof) + sizeof(block->heatmap) +
                         sizeof(block->num_sensor_readings);
    // Calculate dynamic sensor portion size.
    size_t sensors_size = block->num_sensor_readings * sensor_size;
    size_t total_size = header_size + sensors_size;
    
    uint8_t *buffer = malloc(total_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate serialization buffer");
        return 0;
    }
    size_t offset = 0;
    // Serialize header.
    memcpy(buffer + offset, &block->block_num, sizeof(block->block_num));
    offset += sizeof(block->block_num);
    memcpy(buffer + offset, &block->timestamp, sizeof(block->timestamp));
    offset += sizeof(block->timestamp);
    memcpy(buffer + offset, block->prev_hash, sizeof(block->prev_hash));
    offset += sizeof(block->prev_hash);
    memcpy(buffer + offset, block->hash, sizeof(block->hash));
    offset += sizeof(block->hash);
    memcpy(buffer + offset, block->pop_proof, sizeof(block->pop_proof));
    offset += sizeof(block->pop_proof);
    memcpy(buffer + offset, block->heatmap, sizeof(block->heatmap));
    offset += sizeof(block->heatmap);
    memcpy(buffer + offset, &block->num_sensor_readings, sizeof(block->num_sensor_readings));
    offset += sizeof(block->num_sensor_readings);
    
    // Serialize each sensor record in order.
    sensor_record_t *cur = block->node_data;
    while (cur) {
        memcpy(buffer + offset, cur->mac, sizeof(cur->mac));
        offset += sizeof(cur->mac);
        memcpy(buffer + offset, &cur->timestamp, sizeof(cur->timestamp));
        offset += sizeof(cur->timestamp);
        memcpy(buffer + offset, &cur->temperature, sizeof(cur->temperature));
        offset += sizeof(cur->temperature);
        memcpy(buffer + offset, &cur->humidity, sizeof(cur->humidity));
        offset += sizeof(cur->humidity);
        memcpy(buffer + offset, cur->rssi, MAX_NEIGHBORS * sizeof(int8_t));
        offset += MAX_NEIGHBORS * sizeof(int8_t);
        cur = cur->next;
    }
    *out_buffer = buffer;
    return total_size;
}

block_t *blockchain_parse_received_serialized_block(const uint8_t *serialized_data, int payload_len)
{
    // New header: block_num, timestamp, prev_hash, hash, pop_proof, heatmap, num_sensor_readings.
    size_t header_size = sizeof(uint32_t) + sizeof(uint32_t) + 32 + 32 +
                         sizeof(((block_t *)0)->pop_proof) + (HEATMAP_SIZE * sizeof(uint8_t)) +
                         sizeof(uint32_t);
    if ((size_t)payload_len < header_size) {
        ESP_LOGE(TAG, "Received block too short");
        return NULL;
    }
    size_t offset = 0;
    block_t *received_block = malloc(sizeof(block_t));
    if (!received_block) {
        ESP_LOGE(TAG, "Failed to allocate memory for received block");
        return NULL;
    }
    memset(received_block, 0, sizeof(block_t));
    
    // Parse header fields.
    memcpy(&received_block->block_num, serialized_data + offset, sizeof(received_block->block_num));
    offset += sizeof(received_block->block_num);
    memcpy(&received_block->timestamp, serialized_data + offset, sizeof(received_block->timestamp));
    offset += sizeof(received_block->timestamp);
    memcpy(received_block->prev_hash, serialized_data + offset, 32);
    offset += 32;
    memcpy(received_block->hash, serialized_data + offset, 32);
    offset += 32;
    memcpy(received_block->pop_proof, serialized_data + offset, sizeof(received_block->pop_proof));
    offset += sizeof(received_block->pop_proof);
    memcpy(received_block->heatmap, serialized_data + offset, HEATMAP_SIZE);
    offset += HEATMAP_SIZE;
    memcpy(&received_block->num_sensor_readings, serialized_data + offset, sizeof(received_block->num_sensor_readings));
    offset += sizeof(received_block->num_sensor_readings);
    
    // Check that payload length matches header + sensor records.
    size_t expected_size = header_size + (received_block->num_sensor_readings * sensor_size);
    if ((size_t)payload_len != expected_size) {
        ESP_LOGE(TAG, "Received block size mismatch: expected %d, got %d", (int)expected_size, payload_len);
        free(received_block);
        return NULL;
    }
    
    // Parse sensor records.
    sensor_record_t *head = NULL, *tail = NULL;
    for (uint32_t i = 0; i < received_block->num_sensor_readings; i++) {
        sensor_record_t *rec = malloc(sizeof(sensor_record_t));
        if (!rec) {
            ESP_LOGE(TAG, "Failed to allocate memory for sensor record");
            // Free already allocated sensor records.
            sensor_record_t *cur = head;
            while(cur) {
                sensor_record_t *next = cur->next;
                free(cur);
                cur = next;
            }
            free(received_block);
            return NULL;
        }
        memset(rec, 0, sizeof(sensor_record_t));
        memcpy(rec->mac, serialized_data + offset, sizeof(rec->mac));
        offset += sizeof(rec->mac);
        memcpy(&rec->timestamp, serialized_data + offset, sizeof(rec->timestamp));
        offset += sizeof(rec->timestamp);
        memcpy(&rec->temperature, serialized_data + offset, sizeof(rec->temperature));
        offset += sizeof(rec->temperature);
        memcpy(&rec->humidity, serialized_data + offset, sizeof(rec->humidity));
        offset += sizeof(rec->humidity);
        memcpy(rec->rssi, serialized_data + offset, MAX_NEIGHBORS * sizeof(int8_t));
        offset += MAX_NEIGHBORS * sizeof(int8_t);
        rec->next = NULL;
        if (!head) {
            head = rec;
            tail = rec;
        } else {
            tail->next = rec;
            tail = rec;
        }
    }
    received_block->node_data = head;
    return received_block;
}

uint32_t blockchain_init(void)
{
    blockchain_head = NULL;
    blockchain_count = 0;
    blockchain_mutex = xSemaphoreCreateMutex();
    if (!blockchain_mutex) {
        ESP_LOGE(TAG, "Failed to create blockchain mutex");
        return 1;
    }
    ESP_LOGI(TAG, "Blockchain initialized; count = %" PRIu32, blockchain_count);
    return 0;
}

void blockchain_deinit(void)
{
    if (blockchain_mutex && xSemaphoreTake(blockchain_mutex, portMAX_DELAY)) {
        block_t *cur = blockchain_head;
        while (cur) {
            // Free sensor records for this block.
            sensor_record_t *s_record = cur->node_data;
            while (s_record) {
                sensor_record_t *next_record = s_record->next;
                free(s_record);
                s_record = next_record;
            }
            block_t *next_block = cur->next;
            free(cur);
            cur = next_block;
        }
        blockchain_head = NULL;
        blockchain_count = 0;
        xSemaphoreGive(blockchain_mutex);
        vSemaphoreDelete(blockchain_mutex);
        blockchain_mutex = NULL;
    }
    ESP_LOGI(TAG, "Blockchain deinitialized");
}

bool blockchain_add_block(block_t *new_block)
{
    bool result = false;
    if (xSemaphoreTake(blockchain_mutex, portMAX_DELAY)) {
        new_block->next = NULL;
        if (!blockchain_head) {
            // For an empty chain, ensure block_num is 0 already.
            new_block->block_num = 0;
            blockchain_head = new_block;
        } else {
            block_t *cur = blockchain_head;
            while (cur->next) {
                cur = cur->next;
            }
            // Set block_num based on the last block in the chain.
            new_block->block_num = cur->block_num + 1;
            cur->next = new_block;
        }
        blockchain_count++;
        ESP_LOGI(TAG, "Block added; block number = %" PRIu32 ", total count = %" PRIu32, new_block->block_num, blockchain_count);
        result = true;
        xSemaphoreGive(blockchain_mutex);
    }
    return result;
}

bool blockchain_insert_block(block_t *new_block)
{
    bool result = false;
    if (xSemaphoreTake(blockchain_mutex, portMAX_DELAY)) {
        block_t *prev = NULL;
        block_t *cur = blockchain_head;
        while (cur && cur->block_num < new_block->block_num) {
            prev = cur;
            cur = cur->next;
        }
        new_block->next = cur;
        if (!prev) {
            blockchain_head = new_block;
        } else {
            prev->next = new_block;
        }
        blockchain_count++;
        result = true;
        xSemaphoreGive(blockchain_mutex);
    }
    return result;
}

bool blockchain_get_last_block(block_t *block_out)
{
    bool result = false;
    if (xSemaphoreTake(blockchain_mutex, portMAX_DELAY)) {
        if (blockchain_head) {
            block_t *cur = blockchain_head;
            while (cur->next) {
                cur = cur->next;
            }
            memcpy(block_out, cur, sizeof(block_t));
            result = true;
        }
        xSemaphoreGive(blockchain_mutex);
    }
    return result;
}

bool blockchain_get_block_by_number(uint32_t block_num, block_t *block_out)
{
    bool found = false;
    if (xSemaphoreTake(blockchain_mutex, portMAX_DELAY)) {
        block_t *cur = blockchain_head;
        while (cur) {
            if (cur->block_num == block_num) {
                memcpy(block_out, cur, sizeof(block_t));
                found = true;
                break;
            }
            cur = cur->next;
        }
        xSemaphoreGive(blockchain_mutex);
    }
    return found;
}

/**
 * Print the entire blockchain history.
 */
void blockchain_print_history(void)
{
    if (xSemaphoreTake(blockchain_mutex, portMAX_DELAY)) {
        ESP_LOGI(TAG, "===== Blockchain History (Count: %" PRIu32 ") =====", blockchain_count);
        uint32_t count = 0;
        block_t *cur = blockchain_head;
        while (cur) {
            ESP_LOGI(TAG, "Block %" PRIu32 " (global number: %" PRIu32 "):", count++, cur->block_num);
            ESP_LOGI(TAG, "  Prev Hash:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, cur->prev_hash, 32, ESP_LOG_INFO);
            ESP_LOGI(TAG, "  Block Hash:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, cur->hash, 32, ESP_LOG_INFO);
            ESP_LOGI(TAG, "  Timestamp: 0x%" PRIx32, cur->timestamp);
            ESP_LOGI(TAG, "  PoP Proof: %s", cur->pop_proof);
            ESP_LOGI(TAG, "  Sensor Readings (Total: %" PRIu32 "):", cur->num_sensor_readings);
            sensor_record_t *record = cur->node_data;
            while (record) {
                ESP_LOGI(TAG, "    Sensor " MACSTR ": Temp: %.2f°C, Humidity: %.2f%%",
                         MAC2STR(record->mac), record->temperature, record->humidity);
                record = record->next;
            }
            cur = cur->next;
        }
        ESP_LOGI(TAG, "========================================");
        xSemaphoreGive(blockchain_mutex);
    }
}

void blockchain_print_block_struct(block_t *block)
{
    ESP_LOGI(TAG, "Block Number: %" PRIu32, block->block_num);
    ESP_LOGI(TAG, "Timestamp: 0x%" PRIx32, block->timestamp);
    ESP_LOGI(TAG, "Prev Hash:");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, block->prev_hash, 32, ESP_LOG_INFO);
    ESP_LOGI(TAG, "Block Hash:");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, block->hash, 32, ESP_LOG_INFO);
    ESP_LOGI(TAG, "PoP Proof: %s", block->pop_proof);
    ESP_LOGI(TAG, "Sensor Readings (Total: %" PRIu32 "):", block->num_sensor_readings);
    sensor_record_t *record = block->node_data;
    while (record) {
        ESP_LOGI(TAG, "  Sensor " MACSTR ": Temp: %.2f°C, Humidity: %.2f%%",
                 MAC2STR(record->mac), record->temperature, record->humidity);
        record = record->next;
    }
}

// Modify blockchain_create_block to remove dummy merkle root assignment.
void blockchain_create_block(block_t *new_block, sensor_record_t sensor_data[MAX_NODES])
{
    memset(new_block, 0, sizeof(block_t));
    new_block->timestamp = (uint32_t)time(NULL);

    block_t last;
    if (blockchain_get_last_block(&last)) {
        memcpy(new_block->prev_hash, last.hash, sizeof(last.hash));
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
        ESP_LOGI(TAG, "Block with Timestamp 0x%" PRIx32 " received and added", incoming_block.timestamp);
    } else {
        ESP_LOGE(TAG, "Failed to add received block (Timestamp 0x%" PRIx32 ")", incoming_block.timestamp);
    }
}

/**
 * Append a sensor record to the block's linked list.
 */
static void blockchain_append_sensor(block_t *block, const sensor_record_t *record)
{
    sensor_record_t *new_record = malloc(sizeof(sensor_record_t));
    if (!new_record) {
        ESP_LOGE(TAG, "No memory to allocate sensor record");
        return;
    }
    memcpy(new_record, record, sizeof(sensor_record_t));
    new_record->next = NULL;

    if (block->node_data == NULL) {
        block->node_data = new_record;
    } else {
        sensor_record_t *cur = block->node_data;
        while (cur->next) {
            cur = cur->next;
        }
        cur->next = new_record;
    }
    block->num_sensor_readings++;
    ESP_LOGI(TAG, "Added sensor reading: Temp: %.2f, Humidity: %.2f (total: %" PRIu32 ")",
             record->temperature, record->humidity, block->num_sensor_readings);
}

// Helper task to determine if mesh network is formed and we can activate the blockchain receiver task.
void mesh_networking_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting mesh_networking_task");
    while (1) {
        uint32_t node_count = 0;
        const node_info_list_t *list = esp_mesh_lite_get_nodes_list(&node_count);
        if (node_count > 0) {
            ESP_LOGE(TAG, "Mesh network formed with %" PRIu32 " nodes", node_count);
            // Start the blockchain receiver task.
            add_self_broadcast_peer();
            xTaskCreate(sensor_blockchain_task, "sensor_blockchain_task", 4096 * 2, NULL, 5, NULL);
            break;
        } else {
            ESP_LOGI(TAG, "Mesh network not yet formed. Waiting...");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    vTaskDelete(NULL);
}

// In sensor_blockchain_task: update timestamp and calculate hash before Proof-of-participation.
void sensor_blockchain_task(void *pvParameters)
{
    uint8_t elected_leader_mac[ESP_NOW_ETH_ALEN] = {0};

    uint32_t err_status = blockchain_init();
    if (err_status != 0) {
        ESP_LOGE(TAG, "Failed to initialize blockchain");
        vTaskDelete(NULL);
    }
    
    ESP_LOGV(TAG, "Blockchain initialized");
    consensus_init();
    
    // Register ESPNOW receive callback
    ESP_LOGI(TAG, "Registering ESPNOW receive callback");
    esp_mesh_lite_espnow_recv_cb_register(ESPNOW_DATA_TYPE_RESERVE, espnow_recv_cb);

    ESP_LOGI(TAG, "Starting sensor_blockchain_task");

    while (1) {
        uint8_t my_mac[ESP_NOW_ETH_ALEN] = {0};
        esp_wifi_get_mac(ESP_IF_WIFI_STA, my_mac);
        ESP_LOGI(TAG, "My MAC: " MACSTR, MAC2STR(my_mac));

        uint32_t solo_node_count = 0;
        node_info_list_t *solo_list = esp_mesh_lite_get_nodes_list(&solo_node_count);
        if (solo_node_count == 0) {
            ESP_LOGI(TAG, "No nodes in the network. Mesh still forming?");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        if (solo_node_count == 1) {
            ESP_LOGI(TAG, "Only one node in the network. Acting as leader.");
            memcpy(elected_leader_mac, solo_list->node->mac_addr, ESP_NOW_ETH_ALEN);
        }
        
        ESP_LOGW(TAG, "Elected Leader MAC: " MACSTR, MAC2STR(elected_leader_mac));
        ESP_LOGW(TAG, "My MAC: " MACSTR, MAC2STR(my_mac));
        if (consensus_am_i_leader(elected_leader_mac)) {
            ESP_LOGI(TAG, "I am the leader. Initiating sensor data collection.");
            uint8_t pulse_cmd = CMD_PULSE;
            uint32_t node_count = 0;
            const node_info_list_t *list = esp_mesh_lite_get_nodes_list(&node_count);
            
            // Dynamically allocate a new block.
            block_t *new_block = malloc(sizeof(block_t));
            if (!new_block) {
                ESP_LOGE(TAG, "Failed to allocate memory for new block");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            memset(new_block, 0, sizeof(block_t));
            new_block->timestamp = (uint32_t)time(NULL);
                        
            // Determine block number and prev_hash from the existing chain.
            block_t last;
            if (blockchain_get_last_block(&last)) {
                new_block->block_num = last.block_num + 1;
                memcpy(new_block->prev_hash, last.hash, sizeof(last.hash));
            } else {
                new_block->block_num = 0;
                memset(new_block->prev_hash, 0, sizeof(new_block->prev_hash));
            }
            new_block->node_data = NULL;
            new_block->num_sensor_readings = 0;
            
            // Append leader's own sensor reading.
            sensor_record_t my_sensor = {0};
            memcpy(my_sensor.mac, my_mac, ESP_NOW_ETH_ALEN);
            my_sensor.timestamp = (uint32_t)time(NULL);
            my_sensor.temperature = temperature_probe_read_temperature();
            my_sensor.humidity = temperature_probe_read_humidity();
            my_sensor.next = NULL;
            blockchain_append_sensor(new_block, &my_sensor); // count now = 1

            // For each node (excluding leader), send a pulse and wait for response.
            while (list) {
                ESP_LOGI(TAG, "Sending pulse to " MACSTR, MAC2STR(list->node->mac_addr));
                esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, list->node->mac_addr,
                                                    &pulse_cmd, sizeof(pulse_cmd));
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send pulse to " MACSTR, MAC2STR(list->node->mac_addr));
                }
                sensor_record_t response = {0};
                if (waitForNodeResponse(list->node->mac_addr, &response, pdMS_TO_TICKS(5000))) {
                    ESP_LOGI(TAG, "Received sensor data from " MACSTR ": Temp: %.2f°C, Humidity: %.2f%%",
                             MAC2STR(list->node->mac_addr), response.temperature, response.humidity);
                    blockchain_append_sensor(new_block, &response);
                } else {
                    ESP_LOGE(TAG, "No response from " MACSTR, MAC2STR(list->node->mac_addr));
                }
                list = list->next;
            }
            ESP_LOGI(TAG, "All sensor responses processed: total sensors = %" PRIu32,
                     new_block->num_sensor_readings);
            
            // Generate Proof-of-participation.
            consensus_generate_pop_proof(new_block, my_mac);
            // Calculate new block hash.
            calculate_block_hash(new_block);
            ESP_LOGI(TAG, "Prev Hash: ");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, new_block->prev_hash, 32, ESP_LOG_INFO);
            ESP_LOGI(TAG, "Block Hash: ");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, new_block->hash, 32, ESP_LOG_INFO);
            
            // Add block to blockchain.
            blockchain_add_block(new_block);
            ESP_LOGI(TAG, "Block added to blockchain");
            sensor_record_t *record = new_block->node_data;
            while (record) {
                ESP_LOGI(TAG, "    Sensor " MACSTR ": Temp: %.2f°C, Humidity: %.2f%%",
                         MAC2STR(record->mac), record->temperature, record->humidity);
                record = record->next;
            }
            
            // Broadcast the new block to all nodes.
            uint8_t bcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            uint8_t *serialized_block = NULL;
            size_t block_size = blockchain_serialize_block(new_block, &serialized_block);
            if (block_size == 0) {
                ESP_LOGE(TAG, "Failed to serialize new block");
            } else {
                size_t send_buffer_size = 1 + block_size;
                uint8_t *send_buffer = malloc(send_buffer_size);
                if (send_buffer) {
                    send_buffer[0] = CMD_NEW_BLOCK;
                    memcpy(send_buffer + 1, serialized_block, block_size);
                    esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, bcast_mac,
                                                        send_buffer, send_buffer_size);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to broadcast new block: %s", esp_err_to_name(ret));
                    }
                    free(send_buffer);
                } else {
                    ESP_LOGE(TAG, "Failed to allocate send buffer");
                }
                free(serialized_block);
            }
            
            vTaskDelay(pdMS_TO_TICKS(500));
            
            // Election process: current leader obtains the full list, randomly selects one node,
            // then broadcasts that node's MAC address as the next leader.
            node_count = 0;
            node_info_list_t *node_list = esp_mesh_lite_get_nodes_list(&node_count);
            ESP_LOGI(TAG, "Election Process: Current leader broadcasts next leader selection");
            ESP_LOGI(TAG, "Node count: %" PRIu32 ", node_list: %p", node_count, node_list);
            if (node_list != NULL && node_count > 0) 
            {
                uint32_t index = rand() % node_count;
                node_info_list_t *selected = node_list;
                for (uint32_t i = 0; i < index; i++) {
                    selected = selected->next;
                } 
                // Build binary election message: [CMD_ELECTION][6-byte selected MAC]
                uint8_t election_msg[1 + ESP_NOW_ETH_ALEN];
                election_msg[0] = CMD_ELECTION;
                memcpy(election_msg + 1, selected->node->mac_addr, ESP_NOW_ETH_ALEN);
                // Setting our own record of the elected node
                memcpy(elected_leader_mac, selected->node->mac_addr, ESP_NOW_ETH_ALEN);
                ESP_LOGI(TAG, "Selected next leader: " MACSTR, MAC2STR(selected->node->mac_addr));
                ESP_LOGI(TAG, "My MAC: " MACSTR, MAC2STR(my_mac));
                // Broadcast the election message using the broadcast MAC address.
                uint8_t bcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                esp_err_t ret_e = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, bcast_mac,
                                                      election_msg, sizeof(election_msg));
                if(ret_e != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to broadcast election message, err: %s", esp_err_to_name(ret_e));
                }
            } 
            else 
            {
                ESP_LOGW(TAG, "No nodes available for election");
            }
        } 
        else 
        {
            ESP_LOGI(TAG, "Not leader, waiting for election broadcast.");
            if (waitForElectionMessage(elected_leader_mac, pdMS_TO_TICKS(70000))) 
            {
                ESP_LOGI(TAG, "Election message received: next leader MAC = " MACSTR, MAC2STR(elected_leader_mac));
                if (memcmp(elected_leader_mac, my_mac, ESP_NOW_ETH_ALEN) == 0) 
                {
                    ESP_LOGI(TAG, "I am elected as next leader. Preparing to lead next round.");
                    // (Optionally trigger leader initialization.)
                } 
                else 
                {
                    ESP_LOGI(TAG, "Awaiting block broadcast from elected leader.");
                    // Block broadcast reception is handled elsewhere (e.g. via espnow_recv_cb -> blockchain_receive_block)
                }
            } 
            else 
            {
                ESP_LOGW(TAG, "No election message received within timeout. Initiating leader discovery.");
                uint8_t zero_mac[ESP_NOW_ETH_ALEN] = {0};
                if (memcmp(elected_leader_mac, zero_mac, ESP_NOW_ETH_ALEN) == 0) 
                {
                    memcpy(elected_leader_mac, my_mac, ESP_NOW_ETH_ALEN);
                }
                // Build binary election message: [CMD_ELECTION][elected_leader_mac]
                uint8_t election_msg[1 + ESP_NOW_ETH_ALEN];
                election_msg[0] = CMD_ELECTION;
                memcpy(election_msg + 1, elected_leader_mac, ESP_NOW_ETH_ALEN);
                uint8_t bcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, bcast_mac, election_msg, sizeof(election_msg));
                if (ret != ESP_OK) 
                {
                    ESP_LOGE(TAG, "Failed to send election message, err: %s", esp_err_to_name(ret));
                } 
                else 
                {
                    ESP_LOGI(TAG, "Sent election message");
                }
                // Wait additional time for potential leader acknowledgment.
                vTaskDelay(pdMS_TO_TICKS(5000));
                if (!waitForElectionMessage(elected_leader_mac, pdMS_TO_TICKS(5000))) 
                {
                    if (esp_mesh_lite_get_level() <= 1) { // Assuming root if level 0 or 1.
                        ESP_LOGW(TAG, "No leader discovered. Triggering election as root.");
                        uint32_t node_count = 0;
                        node_info_list_t *node_list = esp_mesh_lite_get_nodes_list(&node_count);
                        if (node_list != NULL && node_count > 0) {
                            uint32_t index = rand() % node_count;
                            node_info_list_t *selected = node_list;
                            for (uint32_t i = 0; i < index; i++) 
                            {
                                selected = selected->next;
                            }
                            // Build root election message: [CMD_ELECTION][selected node MAC]
                            uint8_t election_msg_root[1 + ESP_NOW_ETH_ALEN];
                            election_msg_root[0] = CMD_ELECTION;
                            memcpy(election_msg_root + 1, selected->node->mac_addr, ESP_NOW_ETH_ALEN);
                            ESP_LOGI(TAG, "Initiating root election with MAC " MACSTR, MAC2STR(selected->node->mac_addr));
                            ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, bcast_mac, election_msg_root, sizeof(election_msg_root));
                            if(ret != ESP_OK) 
                            {
                                ESP_LOGE(TAG, "Failed to broadcast election message, err: %s", esp_err_to_name(ret));
                            }
                        } 
                        else 
                        {
                            ESP_LOGW(TAG, "No nodes available for election");
                        }
                    }
                }
            }
        }
        // blockchain_print_history();

        // Wait for 15 seconds before generating the next block.
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}
