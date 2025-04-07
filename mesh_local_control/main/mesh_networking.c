#include "mesh_networking.h"
#include "node_response.h"
#include "election_response.h"
#include "command_set.h"

static const char *TAG = "mesh_networking";

uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    ESP_LOGW(TAG, "Received data from " MACSTR, MAC2STR(mac_addr));

    if (len < 1) return; // must have at least the command byte
    uint8_t cmd = data[0];
    switch (cmd) {
        case CMD_ACK:
            ESP_LOGI(TAG, "Got ACK from " MACSTR, MAC2STR(mac_addr));
            break;
        case CMD_PULSE:
            {
                // Received pulse from leader: take a sensor reading.
                float temp = temperature_probe_read_temperature();
                float humidity = temperature_probe_read_humidity();
                uint32_t now = (uint32_t)time(NULL);
                // Build binary sensor message: [CMD_SENSOR_DATA][temp][humidity][timestamp]
                uint8_t sensor_msg[1 + sizeof(float)*2 + sizeof(uint32_t)];
                sensor_msg[0] = CMD_SENSOR_DATA;
                size_t offset = 1;
                memcpy(sensor_msg + offset, &temp, sizeof(float));
                offset += sizeof(float);
                memcpy(sensor_msg + offset, &humidity, sizeof(float));
                offset += sizeof(float);
                memcpy(sensor_msg + offset, &now, sizeof(uint32_t));
                // Broadcast sensor data.
                esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, broadcast_mac,
                                                    sensor_msg, sizeof(sensor_msg));
                if(ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to broadcast sensor data: " MACSTR, MAC2STR(broadcast_mac));
                } else {
                    ESP_LOGI(TAG, "Sensor data broadcasted");
                }
            }
            break;
        case CMD_CHAIN_REQ:
            {
                // Another node requests the blockchain.
                if (consensus_am_i_leader(0)) { // Replace with proper leader status check.
                    const char reply_text[] = "Blockchain syncing not implemented";
                    uint8_t reply[1 + sizeof(reply_text)];
                    reply[0] = CMD_CHAIN_RESP;
                    memcpy(reply + 1, reply_text, sizeof(reply_text));
                    esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, mac_addr,
                                                        reply, sizeof(reply));
                    if(ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to send blockchain sync response to " MACSTR, MAC2STR(mac_addr));
                    }
                }
            }
            break;
        case CMD_ELECTION:
            {
                // Payload (after first byte) should be 6 bytes MAC.
                if (len < 1 + ESP_NOW_ETH_ALEN) {
                    ESP_LOGE(TAG, "Election message too short from " MACSTR, MAC2STR(mac_addr));
                    break;
                }
                uint8_t leader_mac[ESP_NOW_ETH_ALEN];
                memcpy(leader_mac, data + 1, ESP_NOW_ETH_ALEN);
                election_response_push(mac_addr, leader_mac);
                ESP_LOGE(TAG, "Election message from " MACSTR ": next leader MAC = " MACSTR,
                         MAC2STR(mac_addr), MAC2STR(leader_mac));
            }
            break;
        case CMD_NEW_BLOCK:
            {
                // Data after first byte is the serialized block.
                const uint8_t *serialized_data = data + 1;
                int payload_len = len - 1;

                block_t *received_block = blockchain_parse_received_serialized_block(serialized_data, payload_len);
                if (!received_block) {
                    return;
                }               
                
                // Create a temporary copy of the block to validate hash.
                block_t temp_block = *received_block;
                memset(temp_block.hash, 0, sizeof(temp_block.hash));
                ESP_LOGW(TAG, "Temp Block:");
                blockchain_print_block_struct(&temp_block);
                
                ESP_LOGW(TAG, "Recv Block:");
                blockchain_print_block_struct(received_block);
            
                // Compute and validate the block hash.
                calculate_block_hash(&temp_block);
                ESP_LOGW(TAG, "Hash computed for temp block:");
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, temp_block.hash, 32, ESP_LOG_INFO);
                
                if (memcmp(temp_block.hash, received_block->hash, 32) != 0) {
                    ESP_LOGE(TAG, "Block hash validation failed!");
                    ESP_LOG_BUFFER_HEX_LEVEL(TAG, temp_block.hash, 32, ESP_LOG_INFO);
                    ESP_LOG_BUFFER_HEX_LEVEL(TAG, received_block->hash, 32, ESP_LOG_INFO);
                    // Free allocated sensor records and block before returning.
                    sensor_record_t *cur = received_block->node_data;
                    while (cur) {
                        sensor_record_t *next = cur->next;
                        free(cur);
                        cur = next;
                    }
                    free(received_block);
                    return;
                } else {
                    ESP_LOGI(TAG, "Block hash validated successfully.");
                }
                
                ESP_LOGI(TAG, "Adding new block:");
                blockchain_print_block_struct(received_block);
                
                // Add the block to the blockchain (which now expects a pointer).
                blockchain_add_block(received_block);
           
                block_t last_block;
                if (blockchain_get_last_block(&last_block)) {
                    uint32_t expected_num = last_block.block_num + 1;
                    ESP_LOGI(TAG, "Expected block number: %" PRIu32, expected_num);
                    if (received_block->block_num == expected_num) {
                        ESP_LOGI(TAG, "Block number matches expected.");
                    } else {
                        ESP_LOGW(TAG, "Block number mismatch. Expected: %" PRIu32 ", got: %" PRIu32, expected_num, received_block->block_num);
                    }
                    if (received_block->block_num != expected_num) {
                        ESP_LOGW(TAG, "Block number mismatch. Expected: %" PRIu32 ", got: %" PRIu32, expected_num, received_block->block_num);
                        if (received_block->block_num > expected_num) {
                            uint8_t send_buffer[1 + sizeof(uint32_t)];
                            send_buffer[0] = CMD_REQUEST_SPECIFIC_BLOCK;
                            uint32_t missing_block_num = expected_num;
                            memcpy(send_buffer + 1, &missing_block_num, sizeof(uint32_t));
                            espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, broadcast_mac, send_buffer, sizeof(send_buffer));
                        }
                    }
                }

            }
            break;
        case CMD_SENSOR_DATA:
            {
                // Expect payload: [CMD_SENSOR_DATA][float temp][float humidity][uint32_t timestamp]
                if (len != 1 + sizeof(float)*2 + sizeof(uint32_t)) {
                    ESP_LOGE(TAG, "Invalid sensor data length from " MACSTR, MAC2STR(mac_addr));
                    break;
                }
                sensor_record_t sensorData = {0};
                memcpy(sensorData.mac, mac_addr, ESP_NOW_ETH_ALEN);
                size_t offset = 1;
                memcpy(&sensorData.temperature, data + offset, sizeof(float));
                offset += sizeof(float);
                memcpy(&sensorData.humidity, data + offset, sizeof(float));
                offset += sizeof(float);
                memcpy(&sensorData.timestamp, data + offset, sizeof(uint32_t));
                node_response_push(mac_addr, &sensorData);
                ESP_LOGI(TAG, "Received sensor data from " MACSTR, MAC2STR(mac_addr));
            }
            break;
        case CMD_RESET_BLOCKCHAIN:
            ESP_LOGI(TAG, "Received reset command from " MACSTR, MAC2STR(mac_addr));
            blockchain_deinit();
            blockchain_init();
            break;
        case CMD_REQUEST_SPECIFIC_BLOCK:
            {
                ESP_LOGI(TAG, "Received request for specific block from " MACSTR, MAC2STR(mac_addr));
                if (len < 1 + sizeof(uint32_t)) {
                    ESP_LOGE(TAG, "Invalid request length");
                    break;
                }
                uint32_t requested_block_num;
                memcpy(&requested_block_num, data + 1, sizeof(requested_block_num));
                ESP_LOGI(TAG, "Missing block requested: %" PRIu32, requested_block_num);

                // If current node is root, get the block and broadcast it
                if (esp_mesh_lite_get_level() <= 1) {
                    block_t missing_block;
                    if (blockchain_get_block_by_number(requested_block_num, &missing_block)) {
                        // Serialize and broadcast as CMD_HISTORICAL_BLOCK
                        uint8_t *serialized_block = NULL;
                        size_t block_size = blockchain_serialize_block(&missing_block, &serialized_block);
                        if (block_size > 0) {
                            uint8_t send_buffer[1 + block_size];
                            send_buffer[0] = CMD_HISTORICAL_BLOCK;
                            memcpy(send_buffer + 1, serialized_block, block_size);
                            espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, broadcast_mac, send_buffer, sizeof(send_buffer));
                            free(serialized_block);
                        }
                    } else {
                        ESP_LOGW(TAG, "Requested block not found");
                    }
                }
                break;
            }
        case CMD_HISTORICAL_BLOCK:
            {
                // Parse the received block, validate hash, then insert in correct place
                const uint8_t *serialized_data = data + 1;
                int payload_len = len - 1;
                block_t *received_block = blockchain_parse_received_serialized_block(serialized_data, payload_len);
                if (!received_block) { break; }
                
                // Create a temporary copy of the block to validate hash.
                block_t temp_block = *received_block;
                memset(temp_block.hash, 0, sizeof(temp_block.hash));
                calculate_block_hash(&temp_block);
                
                if (memcmp(temp_block.hash, received_block->hash, 32) != 0) {
                    ESP_LOGE(TAG, "Historical block hash validation failed!");
                    free(received_block);
                    break;
                } else {
                    ESP_LOGI(TAG, "Historical block hash validated successfully.");
                }

                ESP_LOGI(TAG, "Adding new block:");
                blockchain_print_block_struct(received_block);

                if (!blockchain_insert_block(received_block)) {
                    ESP_LOGE(TAG, "Failed to insert historical block");
                    free(received_block);
                }
                break;
            }
        default:
            ESP_LOGI(TAG, "Received unknown command 0x%02x from " MACSTR, cmd, MAC2STR(mac_addr));
            break;
    }
}

void add_self_broadcast_peer(void)
{
    esp_now_peer_info_t peerInfo = {0};
    peerInfo.ifidx = WIFI_IF_STA;
    peerInfo.encrypt = false;
    memcpy(peerInfo.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    esp_err_t err = esp_now_add_peer(&peerInfo);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Added broadcast peer on AP interface successfully");
    } else {
        ESP_LOGE(TAG, "Failed adding broadcast peer: %s", esp_err_to_name(err));
    }

    // Add self to receive our own ESPNOW messages
    uint8_t self_mac[6] = {0};
    esp_wifi_get_mac(ESP_IF_WIFI_STA, self_mac);
    esp_now_peer_info_t peerInfoSelf = {0};
    peerInfoSelf.ifidx = WIFI_IF_STA;
    peerInfoSelf.encrypt = false;
    memcpy(peerInfoSelf.peer_addr, self_mac, ESP_NOW_ETH_ALEN);
    esp_err_t err_self = esp_now_add_peer(&peerInfoSelf);
    if (err_self == ESP_OK) {
        ESP_LOGI(TAG, "Added self peer: " MACSTR, MAC2STR(self_mac));
    } else {
        ESP_LOGE(TAG, "Failed adding self peer: %s", esp_err_to_name(err_self));
    }
}

#define ROOT_LEVEL 1
void espnow_periodic_send_task(void *arg)
{
    while (1) {
        uint8_t mesh_level = esp_mesh_lite_get_level();
        ESP_LOGI(TAG, "Mesh level: %d", mesh_level);
        if (mesh_level <= ROOT_LEVEL) {  // Root node has level 0
            const char *msg = "Hello from local_control";
            uint32_t node_count = 0;
            const node_info_list_t *list = esp_mesh_lite_get_nodes_list(&node_count);
            while (list) {
                ESP_LOGI(TAG, "Sending ESPNOW msg to " MACSTR, MAC2STR(list->node->mac_addr));
                esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, list->node->mac_addr, (const uint8_t*)msg, strlen(msg));
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send ESPNOW msg to " MACSTR ", ret=0x%x:%s", MAC2STR(list->node->mac_addr), ret, esp_err_to_name(ret));
                }
                list = list->next;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

esp_err_t espnow_send_wrapper(uint8_t type, const uint8_t *dest_addr, const uint8_t *data, size_t len)
{
    esp_err_t ret = esp_mesh_lite_espnow_send(type, dest_addr, data, len);
    if (ret == ESP_ERR_ESPNOW_NOT_FOUND) {
        ESP_LOGI(TAG, "Peer not found, adding new peer: " MACSTR, MAC2STR(dest_addr));
        esp_now_peer_info_t peerInfo = {0};
        peerInfo.ifidx = WIFI_IF_STA;
        peerInfo.encrypt = false;
        memcpy(peerInfo.peer_addr, dest_addr, ESP_NOW_ETH_ALEN);
        if ((ret = esp_now_add_peer(&peerInfo)) == ESP_OK) {
            ret = esp_mesh_lite_espnow_send(type, dest_addr, data, len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to re-send ESPNOW msg to " MACSTR ", err=0x%x:%s",
                         MAC2STR(dest_addr), ret, esp_err_to_name(ret));
            }
        } else {
            ESP_LOGE(TAG, "Failed adding peer: %s", esp_err_to_name(ret));
        }
    }
    return ret;
}