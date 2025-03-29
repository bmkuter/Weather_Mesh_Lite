#include "mesh_networking.h"
#include "node_response.h"
#include "election_response.h"

static const char *TAG = "mesh_networking";

uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    if (strncmp((const char *)data, "ACK", len) == 0) {
        ESP_LOGI(TAG, "Got ACK from " MACSTR, MAC2STR(mac_addr));
    }
    else if (strncmp((const char *)data, "PULSE", 5) == 0) {
        // Received pulse from leader: take a sensor reading and reply with sensor data.
        uint16_t node_id = get_my_node_id();
        float temp = temperature_probe_read_temperature();
        float humidity = temperature_probe_read_humidity();
        uint32_t now = (uint32_t)time(NULL);
        char sensor_msg[128];
        snprintf(sensor_msg, sizeof(sensor_msg), "SENSOR_DATA:%.2f:%.2f:%lu", temp, humidity, now);
        esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, mac_addr,
                                            (const uint8_t *)sensor_msg, strlen(sensor_msg));
        if(ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send sensor data to leader " MACSTR, MAC2STR(mac_addr));
        } else {
            ESP_LOGI(TAG, "Sensor data sent to leader: %s", sensor_msg);
        }
    }
    else if (strncmp((const char *)data, "CHAIN_REQ", 9) == 0) {
        // Another node requests the blockchain (e.g., after rejoining).
        // If this node is the leader (dummy check here), respond accordingly.
        if (consensus_am_i_leader(0)) { // Replace 0 with proper logic to determine leader status.
            const char *resp = "CHAIN_RESP:Blockchain syncing not implemented";
            esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, mac_addr,
                                                (const uint8_t *)resp, strlen(resp));
            if(ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send blockchain sync response to " MACSTR, MAC2STR(mac_addr));
            }
        }
    }
    else if (strncmp((const char *)data, "ELECTION:", 9) == 0) {
        uint8_t leader_mac[ESP_NOW_ETH_ALEN] = {0};
        int values[ESP_NOW_ETH_ALEN];
        // Parse message formatted as "ELECTION:XX:XX:XX:XX:XX:XX"
        if (sscanf((const char *)data, "ELECTION:%2x:%2x:%2x:%2x:%2x:%2x",
                   &values[0], &values[1], &values[2],
                   &values[3], &values[4], &values[5]) == ESP_NOW_ETH_ALEN) {
            for (int i = 0; i < ESP_NOW_ETH_ALEN; i++) {
                leader_mac[i] = (uint8_t)values[i];
            }
            election_response_push(mac_addr, leader_mac);
            ESP_LOGI(TAG, "Election message from " MACSTR ": next leader MAC = " MACSTR,
                     MAC2STR(mac_addr), MAC2STR(leader_mac));
        } else {
            ESP_LOGE(TAG, "Failed to parse election message from " MACSTR, MAC2STR(mac_addr));
        }
    }
    else if (strncmp((const char *)data, "NEW_BLOCK", sizeof("NEW_BLOCK")) == 0) {
        block_t * temp_block_ptr = (block_t *) data + sizeof("NEW_BLOCK");
        ESP_LOGI(TAG, "Received new block from " MACSTR, MAC2STR(mac_addr));
        // Print each member of the incoming block_t type.
        ESP_LOGI(TAG, "Block timestamp: %" PRIu32, ((block_t *)temp_block_ptr)->timestamp);
        ESP_LOGI(TAG, "Prev Hash: ");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, temp_block_ptr->prev_hash, 32, ESP_LOG_INFO);
        ESP_LOGI(TAG, "Block Hash: ");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, temp_block_ptr->hash, 32, ESP_LOG_INFO);
        ESP_LOGI(TAG, "Block PoP proof: %.*s", 64, ((block_t *)temp_block_ptr)->pop_proof);
        blockchain_receive_block(data + sizeof("NEW_BLOCK"), sizeof(block_t));
    }
    else if (strncmp((const char *)data, "SENSOR_DATA:", sizeof("SENSOR_DATA:")) == 0){
        // Check for sensor data submission.
        ESP_LOGI(TAG, "Received sensor data from " MACSTR ": %.*s", MAC2STR(mac_addr), len, data);
        sensor_record_t sensorData = {0};
        // Set sender MAC in sensor record.
        memcpy(sensorData.mac, mac_addr, ESP_NOW_ETH_ALEN);
        // Example expected format now: "SENSOR_DATA:%f:%f:%lu"
        sscanf((const char *)data, "SENSOR_DATA:%f:%f:%" PRIu32,
                &sensorData.temperature,
                &sensorData.humidity,
                &sensorData.timestamp);
        // Push the parsed data into the response queue.
        node_response_push(mac_addr, &sensorData);
    } 
    else if (strncmp((const char *)data, "RESET_BLOCKCHAIN", sizeof("RESET_BLOCKCHAIN")))
    {
        // Reset the local blockchain (and trigger a new election as needed).
        blockchain_deinit();
        blockchain_init();
    }
    else 
    {
        ESP_LOGI(TAG, "Received unknown message from " MACSTR ": %.*s", MAC2STR(mac_addr), len, data);
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