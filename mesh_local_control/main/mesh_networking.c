#include "mesh_networking.h"
#include "node_response.h"
#include "election_response.h"
#include "command_set.h"

static const char *TAG = "mesh_networking";

uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
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
                ESP_LOGI(TAG, "Election message from " MACSTR ": next leader MAC = " MACSTR,
                         MAC2STR(mac_addr), MAC2STR(leader_mac));
            }
            break;
        case CMD_NEW_BLOCK:
            {
                // Data after first byte is the serialized block.
                const uint8_t *serialized_data = data + 1;
                int payload_len = len - 1;
                size_t header_size = sizeof(uint32_t) + 32 + sizeof(((block_t *)0)->pop_proof) +
                                     HEATMAP_SIZE * sizeof(uint8_t) + sizeof(uint32_t);
                if (payload_len < header_size) {
                    ESP_LOGE(TAG, "Received block too short");
                    return;
                }
                size_t offset = 0;
                block_t received_block;
                memset(&received_block, 0, sizeof(block_t));
                memcpy(&received_block.timestamp, serialized_data + offset, sizeof(received_block.timestamp));
                offset += sizeof(received_block.timestamp);
                memcpy(received_block.prev_hash, serialized_data + offset, 32);
                offset += 32;
                memcpy(received_block.pop_proof, serialized_data + offset, sizeof(received_block.pop_proof));
                offset += sizeof(received_block.pop_proof);
                memcpy(received_block.heatmap, serialized_data + offset, HEATMAP_SIZE);
                offset += HEATMAP_SIZE;
                memcpy(&received_block.num_sensor_readings, serialized_data + offset, sizeof(received_block.num_sensor_readings));
                offset += sizeof(received_block.num_sensor_readings);
                size_t expected_size = header_size + (received_block.num_sensor_readings * sensor_size);
                if (payload_len != expected_size) {
                    ESP_LOGE(TAG, "Received block size mismatch: expected %d, got %d", (int)(expected_size + 1), len);
                    return;
                }
                sensor_record_t *head = NULL, *tail = NULL;
                for (uint32_t i = 0; i < received_block.num_sensor_readings; i++) {
                    sensor_record_t *rec = malloc(sizeof(sensor_record_t));
                    if (!rec) {
                        ESP_LOGE(TAG, "Failed to allocate memory for sensor record");
                        break;
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
                    if (head == NULL) {
                        head = rec;
                        tail = rec;
                    } else {
                        tail->next = rec;
                        tail = rec;
                    }
                }
                received_block.node_data = head;
                ESP_LOGI(TAG, "Received new block:");
                ESP_LOGI(TAG, "Timestamp: %" PRIu32, received_block.timestamp);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, received_block.prev_hash, 32, ESP_LOG_INFO);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, received_block.hash, 32, ESP_LOG_INFO);
                ESP_LOGI(TAG, "PoP Proof: %s", received_block.pop_proof);
                ESP_LOGI(TAG, "Sensor readings count: %" PRIu32, received_block.num_sensor_readings);
                // Print all sensor records.
                sensor_record_t *cur = received_block.node_data;
                while (cur) {
                    ESP_LOGI(TAG, "Sensor " MACSTR ": Temp: %.2f°C, Humidity: %.2f%%",
                             MAC2STR(cur->mac), cur->temperature, cur->humidity);
                    cur = cur->next;
                }
                blockchain_add_block(&received_block);
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