#include "node_id.h"
#include "esp_wifi.h"

uint16_t get_my_node_id(void)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    // For example, use the last two bytes to form a 16-bit ID.
    return ((uint16_t)mac[4] << 8) | mac[5];
}