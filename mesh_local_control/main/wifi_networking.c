#include "wifi_networking.h"
#include "blockchain.h"
#include "mesh_networking.h"
#include "secrets.h"

static const char *TAG = "wifi_networking";
static int g_sockfd    = -1;

/**
 * @brief Create a tcp client
 */
int socket_tcp_client_create(const char *ip, uint16_t port)
{
    ESP_LOGD(TAG, "Create a tcp client, ip: %s, port: %d", ip, port);

    esp_err_t ret = ESP_OK;
    int sockfd    = -1;
    struct ifreq iface;
    memset(&iface, 0x0, sizeof(iface));
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(ip),
    };

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        ESP_LOGE(TAG, "socket create, sockfd: %d", sockfd);
        goto ERR_EXIT;
    }

    esp_netif_get_netif_impl_name(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), iface.ifr_name);
    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE,  &iface, sizeof(struct ifreq)) != 0) {
        ESP_LOGE(TAG, "Bind [sock=%d] to interface %s fail", sockfd, iface.ifr_name);
    }

    ret = connect(sockfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        ESP_LOGD(TAG, "socket connect, ret: %d, ip: %s, port: %d",
                 ret, ip, port);
        goto ERR_EXIT;
    }
    return sockfd;

ERR_EXIT:

    if (sockfd != -1) {
        close(sockfd);
    }

    return -1;
}

void tcp_server_task(void *arg)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create server socket");
        vTaskDelete(NULL);
    }
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_SERVER_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        close(listen_sock);
        vTaskDelete(NULL);
    }
    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "Socket listen failed");
        close(listen_sock);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "TCP server listening on port %d", CONFIG_SERVER_PORT);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) {
            ESP_LOGE(TAG, "Failed to accept client connection");
            continue;
        }
        char buffer[256];
        int len = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
        if (len > 0) {
            buffer[len] = '\0';
            ESP_LOGI(TAG, "Received: %s", buffer);
            if (!strcmp((char *)buffer, "READ_LEDGER")) {
                blockchain_print_history();
            }
            else if (!strcmp((char *)buffer, "RESET_BLOCKCHAIN")) {
                // Broadcast reset instruction over mesh.
                uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                const char *reset_cmd = "RESET_BLOCKCHAIN";
                esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE,
                                                    broadcast_mac,
                                                    (const uint8_t *)reset_cmd,
                                                    strlen(reset_cmd));
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Reset command broadcast successfully");
                } else {
                    ESP_LOGE(TAG, "Broadcast of reset command failed: %s", esp_err_to_name(ret));
                }
                // Reset the local blockchain (and trigger a new election as needed).
                blockchain_deinit();
                blockchain_init();
            }
            else {
                ESP_LOGW(TAG, "Unknown command: %s", buffer);
            }
        }
        close(client_sock);
    }
    close(listen_sock);
    vTaskDelete(NULL);
}

void ip_event_sta_got_ip_handler(void *arg, esp_event_base_t event_base,
    int32_t event_id, void *event_data)
{
    ESP_LOGE(TAG, "Sta got ip event");
    static bool tcp_task = false;
    
    if (event_id == IP_EVENT_STA_GOT_IP) {
        // Cast event data to the appropriate type
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        esp_netif_ip_info_t ip_info = event->ip_info;
        ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&ip_info.ip));
    }

    if (!tcp_task) {
        xTaskCreate(tcp_server_task, "tcp_server_task", 4 * 1024, NULL, 5, NULL);
        tcp_task = true;
    }
}

void wifi_init(void)
{
    // Station
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = SECRET_SSID,
            .password = SECRET_PASSWORD,
        },
    };
    wifi_config.sta.failure_retry_cnt = 2;
    esp_bridge_wifi_set_config(WIFI_IF_STA, &wifi_config);

    // Softap
    snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%s", CONFIG_BRIDGE_SOFTAP_SSID);
    strlcpy((char *)wifi_config.ap.password, CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(wifi_config.ap.password));
    esp_bridge_wifi_set_config(WIFI_IF_AP, &wifi_config);
}

void app_wifi_set_softap_info(void)
{
    char softap_ssid[32];
    char softap_psw[64];
    uint8_t softap_mac[6];
    size_t size = sizeof(softap_psw);
    esp_wifi_get_mac(WIFI_IF_AP, softap_mac);
    memset(softap_ssid, 0x0, sizeof(softap_ssid));

#ifdef CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC
    snprintf(softap_ssid, sizeof(softap_ssid), "%.25s_%02x%02x%02x", CONFIG_BRIDGE_SOFTAP_SSID, softap_mac[3], softap_mac[4], softap_mac[5]);
#else
    snprintf(softap_ssid, sizeof(softap_ssid), "%.32s", CONFIG_BRIDGE_SOFTAP_SSID);
#endif
    if (esp_mesh_lite_get_softap_ssid_from_nvs(softap_ssid, &size) != ESP_OK) {
        esp_mesh_lite_set_softap_ssid_to_nvs(softap_ssid);
    }
    if (esp_mesh_lite_get_softap_psw_from_nvs(softap_psw, &size) != ESP_OK) {
        esp_mesh_lite_set_softap_psw_to_nvs(CONFIG_BRIDGE_SOFTAP_PASSWORD);
    }
    esp_mesh_lite_set_softap_info(softap_ssid, CONFIG_BRIDGE_SOFTAP_PASSWORD);
}