#include "mesh_networking.h"

static const char *TAG = "mesh_networking";

uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    if (strncmp((const char *)data, "ACK", len) == 0) {
        ESP_LOGI(TAG, "Got ACK from " MACSTR, MAC2STR(mac_addr));
    } else {
        ESP_LOGI(TAG, "Received '%.*s' from " MACSTR ", responding with ACK", len, data, MAC2STR(mac_addr));
        const char *ack = "ACK";
        esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE, (uint8_t *)mac_addr, (const uint8_t *)ack, strlen(ack));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send ACK to " MACSTR ", err=0x%x:%s", MAC2STR(mac_addr), ret, esp_err_to_name(ret));
        }
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