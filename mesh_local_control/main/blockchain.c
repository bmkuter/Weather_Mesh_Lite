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

#define BLOCKCHAIN_BUFFER_SIZE 16  // Maximum blocks stored locally

static const char *TAG = "BLOCKCHAIN";
static block_t *blockchain_buffer = NULL;
static size_t blockchain_size = 0;
static SemaphoreHandle_t blockchain_mutex = NULL;

/**
 * Compute the SHA‑256 hash for the given block.
 * (Temporarily zero out the hash field so it is not included in the hash calculation).
 */
static void calculate_block_hash(block_t *block) {
    // Fixed fields: timestamp, prev_hash, pop_proof, heatmap, and num_sensor_readings.
    size_t fixed_size = sizeof(block->timestamp) + sizeof(block->prev_hash) +
                        sizeof(block->pop_proof) + sizeof(block->heatmap) +
                        sizeof(block->num_sensor_readings);
    // Total size = fixed fields + all sensor records.
    size_t total_size = fixed_size + (block->num_sensor_readings * sensor_size);
    
    uint8_t *serial_buffer = malloc(total_size);
    if (!serial_buffer) {
        ESP_LOGE(TAG, "Failed to allocate serial buffer");
        return;
    }
    size_t offset = 0;
    
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
        memcpy(serial_buffer + offset, cur->rssi, MAX_NEIGHBORS*sizeof(int8_t));
        offset += MAX_NEIGHBORS*sizeof(int8_t);
        
        cur = cur->next;
    }
    
    ESP_LOGI(TAG, "Block serialized for hash calculation, total bytes: %d", total_size);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, serial_buffer, total_size, ESP_LOG_INFO);

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
    // Fixed header size.
    size_t header_size = sizeof(block->timestamp) + sizeof(block->prev_hash) +
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
    memcpy(buffer + offset, &block->timestamp, sizeof(block->timestamp));
    offset += sizeof(block->timestamp);
    memcpy(buffer + offset, block->prev_hash, sizeof(block->prev_hash));
    offset += sizeof(block->prev_hash);
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

void blockchain_deinit(void)
{
    if (blockchain_buffer) {
        free(blockchain_buffer);
        blockchain_buffer = NULL;
    }
    if (blockchain_mutex) {
        vSemaphoreDelete(blockchain_mutex);
        blockchain_mutex = NULL;
    }
    blockchain_size = 0;
    ESP_LOGI(TAG, "Blockchain deinitialized");
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

/**
 * Print the last block.
 */
void blockchain_print_last_block(void)
{
    block_t last;
    if (blockchain_get_last_block(&last)) {
        ESP_LOGI(TAG, "----- Latest Block -----");
        ESP_LOGI(TAG, "Timestamp: 0x%" PRIx32, last.timestamp);
        ESP_LOGI(TAG, "Prev Hash: ");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, last.prev_hash, 32, ESP_LOG_INFO);
        ESP_LOGI(TAG, "Block Hash: ");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, last.hash, 32, ESP_LOG_INFO);
        ESP_LOGI(TAG, "PoP Proof: %s", last.pop_proof);
        for (int i = 0; i < MAX_NODES; i++) {
            sensor_record_t *record = &last.node_data[i];
            if (record->mac[0] != 0) {
                ESP_LOGI(TAG, "  Sensor " MACSTR ": Temp: %.2f°C, Humidity: %.2f%%",
                         MAC2STR(record->mac), record->temperature, record->humidity);
            }
        }
        ESP_LOGI(TAG, "------------------------");
    } else {
        ESP_LOGI(TAG, "No block available");
    }
}

/**
 * Print the entire blockchain history.
 */
void blockchain_print_history(void)
{
    if (xSemaphoreTake(blockchain_mutex, portMAX_DELAY)) {
        ESP_LOGI(TAG, "===== Blockchain History =====");
        for (size_t i = 0; i < blockchain_size; i++) {
            block_t *current = &blockchain_buffer[i];
            ESP_LOGI(TAG, "Block %d:", i);
            ESP_LOGI(TAG, "  Prev Hash:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, current->prev_hash, 32, ESP_LOG_INFO);
            ESP_LOGI(TAG, "  Block Hash:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, current->hash, 32, ESP_LOG_INFO);
            ESP_LOGI(TAG, "  Timestamp: 0x%" PRIx32, current->timestamp);
            ESP_LOGI(TAG, "  PoP Proof: %s", current->pop_proof);
            ESP_LOGI(TAG, "  Sensor Readings (Total: %" PRIu32 "):", current->num_sensor_readings);
            sensor_record_t *record = current->node_data;
            while (record) {
                ESP_LOGI(TAG, "    Sensor " MACSTR ": Temp: %.2f°C, Humidity: %.2f%%",
                         MAC2STR(record->mac), record->temperature, record->humidity);
                record = record->next;
            }
        }
        ESP_LOGI(TAG, "===============================");
        xSemaphoreGive(blockchain_mutex);
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

// In sensor_blockchain_task: update timestamp and calculate hash before Proof-of-participation.
void sensor_blockchain_task(void *pvParameters)
{
    uint8_t elected_leader_mac[ESP_NOW_ETH_ALEN] = {0};

    uint32_t err_status = 0;
    err_status = blockchain_init();
    if (err_status != 0)
    {
        /* code */
        ESP_LOGE(TAG, "Failed to initialize blockchain");
        vTaskDelete(NULL);
    }
    
    ESP_LOGV(TAG, "Blockchain initialized");
    consensus_init();

    ESP_LOGI(TAG, "Starting sensor_blockchain_task");

    while (1) {
        uint8_t my_mac[ESP_NOW_ETH_ALEN] = {0};
        esp_wifi_get_mac(ESP_IF_WIFI_STA, my_mac);
        ESP_LOGI(TAG, "My MAC: " MACSTR, MAC2STR(my_mac));

        uint32_t solo_node_count = 0;
        node_info_list_t *solo_list = esp_mesh_lite_get_nodes_list(&solo_node_count);
        if (solo_node_count == 0)   // No nodes in the network
        {   
            ESP_LOGI(TAG, "No nodes in the network. Mesh still forming?");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        if (solo_node_count == 1)   // Only one node in the network 
        {
            ESP_LOGI(TAG, "Only one node in the network. Acting as leader.");
            memcpy(elected_leader_mac, solo_list->node->mac_addr, ESP_NOW_ETH_ALEN);
        }

        // If our MAC matches the elected leader MAC, we act as leader.
        ESP_LOGW(TAG, "Elected Leader MAC: " MACSTR, MAC2STR(elected_leader_mac));
        ESP_LOGW(TAG, "My MAC: " MACSTR, MAC2STR(my_mac));
        if (consensus_am_i_leader(elected_leader_mac)) 
        {
            ESP_LOGI(TAG, "I am the leader. Initiating sensor data collection.");
            const char *pulse_msg = "PULSE";
            uint32_t node_count = 0;
            const node_info_list_t *list = esp_mesh_lite_get_nodes_list(&node_count);

            // Prepare new block: zero out and set basic fields.
            block_t new_block;
            memset(&new_block, 0, sizeof(block_t));
            new_block.timestamp = (uint32_t)time(NULL);
            block_t last;
            if (blockchain_get_last_block(&last)) {
                memcpy(new_block.prev_hash, last.hash, sizeof(last.hash));
            } else {
                memset(new_block.prev_hash, 0, sizeof(new_block.prev_hash));
            }
            // Initialize the linked list head to NULL and sensor count to 0.
            new_block.node_data = NULL;
            new_block.num_sensor_readings = 0;
            
            // Append leader's own sensor reading.
            sensor_record_t my_sensor = {0};
            memcpy(my_sensor.mac, my_mac, ESP_NOW_ETH_ALEN);
            my_sensor.timestamp = (uint32_t)time(NULL);
            my_sensor.temperature = temperature_probe_read_temperature();
            my_sensor.humidity = temperature_probe_read_humidity();
            my_sensor.next = NULL;
            blockchain_append_sensor(&new_block, &my_sensor); // now count = 1

            // For each node (excluding leader), send a pulse and wait for response.
            while (list) {
                ESP_LOGI(TAG, "Sending pulse to " MACSTR, MAC2STR(list->node->mac_addr));
                esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, list->node->mac_addr,
                                                    (const uint8_t *)pulse_msg, strlen(pulse_msg));
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send pulse to " MACSTR, MAC2STR(list->node->mac_addr));
                }
                sensor_record_t response = {0};
                if (waitForNodeResponse(list->node->mac_addr, &response, pdMS_TO_TICKS(5000))) {
                    ESP_LOGI(TAG, "Received sensor data from " MACSTR ": Temp: %.2f°C, Humidity: %.2f%%",
                             MAC2STR(list->node->mac_addr), response.temperature, response.humidity);
                    blockchain_append_sensor(&new_block, &response);
                }
                else {
                    ESP_LOGE(TAG, "No response from " MACSTR, MAC2STR(list->node->mac_addr));
                }
                list = list->next;
            }
            ESP_LOGI(TAG, "All sensor responses processed");
            
            // Calculate new block hash and continue as before...
            calculate_block_hash(&new_block);
            // Generate Proof-of-participation.
            consensus_generate_pop_proof(&new_block, my_mac);
            ESP_LOGI(TAG, "Prev Hash: ");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, new_block.prev_hash, 32, ESP_LOG_INFO);
            ESP_LOGI(TAG, "Block Hash: ");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, new_block.hash, 32, ESP_LOG_INFO);
            // Add block to blockchain.
            blockchain_add_block(&new_block);

            // Broadcast the new block to all nodes.
            uint8_t bcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            // Pack block payload with a command prefix.
            char new_block_command[] = "NEW_BLOCK";
            // Serialize the block.
            uint8_t *serialized_block = NULL;
            size_t block_size = blockchain_serialize_block(&new_block, &serialized_block);
            if (block_size == 0) {
                ESP_LOGE(TAG, "Failed to serialize new block");
            } else {
                // Allocate a send buffer = command size + serialized block.
                size_t send_buffer_size = sizeof(new_block_command) + block_size;
                uint8_t *send_buffer = malloc(send_buffer_size);
                if (send_buffer) {
                    memcpy(send_buffer, new_block_command, sizeof(new_block_command));
                    memcpy(send_buffer + sizeof(new_block_command), serialized_block, block_size);
                    // Broadcast using ESPNOW.
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
                char election_msg[64];
                // Format election message with the selected node's MAC address.
                // MACSTR expects 6 hex values.
                snprintf(election_msg, sizeof(election_msg), "ELECTION:" MACSTR,
                            MAC2STR(selected->node->mac_addr));
                // Broadcast the election message using the broadcast MAC address.
                uint8_t bcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                ESP_LOGI(TAG, "Broadcasting election message: %s", election_msg);
                esp_err_t ret_e = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, bcast_mac,
                                                        (const uint8_t *)election_msg, strlen(election_msg));
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

                char election_msg[64];
                uint8_t zero_mac[ESP_NOW_ETH_ALEN] = {0};
                if (memcmp(elected_leader_mac, zero_mac, ESP_NOW_ETH_ALEN) == 0) 
                {
                    memcpy(elected_leader_mac, my_mac, ESP_NOW_ETH_ALEN);
                }
                snprintf(election_msg, sizeof(election_msg), "ELECTION:" MACSTR, MAC2STR(elected_leader_mac));
                uint8_t bcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, bcast_mac, (const uint8_t *)election_msg, strlen(election_msg));
                if (ret != ESP_OK) 
                {
                    ESP_LOGE(TAG, "Failed to send election message, err: %s", esp_err_to_name(ret));
                } 
                else 
                {
                    ESP_LOGI(TAG, "Sent election message: %s", election_msg);
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
                            char elect_msg[64];
                            snprintf(elect_msg, sizeof(elect_msg), "ELECTION:" MACSTR, MAC2STR(selected->node->mac_addr));
                            ESP_LOGI(TAG, "Initiating root election with MAC " MACSTR, MAC2STR(selected->node->mac_addr));
                            ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, bcast_mac, (const uint8_t *)elect_msg, strlen(elect_msg));
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

        // Wait for 60 seconds before generating the next block.
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}