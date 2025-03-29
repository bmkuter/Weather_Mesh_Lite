#include "ws_comm.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mesh_networking.h"
#include "blockchain.h"
#include "esp_mesh_lite.h"
#include <string.h>

static const char *TAG = "WS_COMM";

/**
 * @brief WebSocket event handler.
 *
 * This handler is invoked for incoming websocket frames on the /ws URI.
 */
static esp_err_t ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
    uint8_t buf[256] = {0};
    ws_pkt.payload = buf;
    ws_pkt.len = sizeof(buf);

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to receive websocket frame");
        return ret;
    }
    
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        buf[ws_pkt.len] = '\0';
        ESP_LOGI(TAG, "Received WS command: %s", (char *)buf);
        
        if (!strcmp((char *)buf, "READ_LEDGER")) {
            blockchain_print_history();
            const char *resp = "Ledger printed to log";
            httpd_ws_frame_t out_pkt;
            memset(&out_pkt, 0, sizeof(httpd_ws_frame_t));
            out_pkt.payload = (uint8_t *)resp;
            out_pkt.len = strlen(resp);
            out_pkt.type = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame(req, &out_pkt);
        }
        else if (!strcmp((char *)buf, "RESET_BLOCKCHAIN")) {
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
            // You could add a call here to trigger new election for the first block.
            const char *resp = "Blockchain has been reset";
            httpd_ws_frame_t out_pkt;
            memset(&out_pkt, 0, sizeof(httpd_ws_frame_t));
            out_pkt.payload = (uint8_t *)resp;
            out_pkt.len = strlen(resp);
            out_pkt.type = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame(req, &out_pkt);
        }
        else {
            const char *resp = "Unknown command";
            httpd_ws_frame_t out_pkt;
            memset(&out_pkt, 0, sizeof(httpd_ws_frame_t));
            out_pkt.payload = (uint8_t *)resp;
            out_pkt.len = strlen(resp);
            out_pkt.type = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame(req, &out_pkt);
        }
    }
    return ESP_OK;
}

static httpd_uri_t ws = {
    .uri       = "/ws",
    .method    = HTTP_GET,
    .handler   = ws_handler,
    .user_ctx  = NULL,
    .is_websocket = true,
};

static httpd_handle_t server = NULL;

void ws_comm_init(void)
{
    ESP_LOGI(TAG, "WebSocket communication interface initialized");
}

void ws_comm_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Listen on port 80 (non-secure)
    config.server_port = 80;
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WebSocket server started on port %d", config.server_port);
        httpd_register_uri_handler(server, &ws);
    } else {
        ESP_LOGE(TAG, "Failed to start WebSocket server: %s", esp_err_to_name(ret));
    }
}