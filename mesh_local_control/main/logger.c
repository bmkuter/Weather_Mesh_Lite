#include "logger.h"

static const char *TAG = "logger";

/**
 * @brief Timed printing system information
 */
void print_system_info_timercb(TimerHandle_t timer)
{
    uint8_t primary                 = 0;
    uint8_t sta_mac[6]              = {0};
    wifi_ap_record_t ap_info        = {0};
    wifi_second_chan_t second       = 0;
    wifi_sta_list_t wifi_sta_list   = {0x0};

    esp_wifi_sta_get_ap_info(&ap_info);
    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
    esp_wifi_ap_get_sta_list(&wifi_sta_list);
    esp_wifi_get_channel(&primary, &second);

    ESP_LOGW(TAG, "System information, channel: %d, layer: %d, self mac: " MACSTR ", parent bssid: " MACSTR
             ", parent rssi: %d, free heap: %"PRIu32"", primary,
             esp_mesh_lite_get_level(), MAC2STR(sta_mac), MAC2STR(ap_info.bssid),
             (ap_info.rssi != 0 ? ap_info.rssi : -120), esp_get_free_heap_size());
#if CONFIG_MESH_LITE_MAXIMUM_NODE_NUMBER
    ESP_LOGW(TAG, "child node number: %lu", esp_mesh_lite_get_child_node_number());
#endif /* MESH_LITE_NODE_INFO_REPORT */
    for (int i = 0; i < wifi_sta_list.num; i++) {
        ESP_LOGW(TAG, "Child mac: " MACSTR, MAC2STR(wifi_sta_list.sta[i].mac));
    }
#if CONFIG_MESH_LITE_NODE_INFO_REPORT
    uint32_t node_count = 0;
    const node_info_list_t *list = esp_mesh_lite_get_nodes_list(&node_count);
    if (node_count > 0) {
        ESP_LOGI(TAG, "=== Mesh Node List ===");
        while (list) {
            ESP_LOGI(TAG, "MAC: " MACSTR ", Level: %d", MAC2STR(list->node->mac_addr), (int)list->node->level);
            list = list->next;
        }
    }
#endif
}