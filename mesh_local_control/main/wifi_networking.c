#include "wifi_networking.h"
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

void tcp_client_write_task(void *arg)
{
    size_t size        = 0;
    int count          = 0;
    char *data         = NULL;
    esp_err_t ret      = ESP_OK;
    uint8_t sta_mac[6] = {0};

    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);

    ESP_LOGI(TAG, "TCP client write task is running");

    while (1) {
        if (g_sockfd == -1) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            g_sockfd = socket_tcp_client_create(CONFIG_SERVER_IP, CONFIG_SERVER_PORT);
            continue;
        }

        vTaskDelay(3000 / portTICK_PERIOD_MS);

        size = asprintf(&data, "{\"src_addr\": \"" MACSTR "\",\"data\": \"Hello TCP Server!\",\"level\": %d,\"count\": %d}\r\n",
                        MAC2STR(sta_mac), esp_mesh_lite_get_level(), count++);

        ESP_LOGD(TAG, "TCP write, size: %d, data: %s", size, data);
        ret = write(g_sockfd, data, size);
        free(data);

        if (ret <= 0) {
            ESP_LOGE(TAG, "<%s> TCP write", strerror(errno));
            close(g_sockfd);
            g_sockfd = -1;
            continue;
        }
    }

    ESP_LOGI(TAG, "TCP client write task is exit");

    close(g_sockfd);
    g_sockfd = -1;
    if (data) {
        free(data);
    }
    vTaskDelete(NULL);
}

void ip_event_sta_got_ip_handler(void *arg, esp_event_base_t event_base,
    int32_t event_id, void *event_data)
{
    ESP_LOGE(TAG, "Sta got ip event");
    static bool tcp_task = false;

    if (!tcp_task) {
        xTaskCreate(tcp_client_write_task, "tcp_client_write_task", 4 * 1024, NULL, 5, NULL);
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