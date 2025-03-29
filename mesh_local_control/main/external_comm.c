#include "external_comm.h"
#include "mqtt_client.h"
#include "mesh_networking.h"
#include "esp_mesh_lite.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "EXTERNAL_COMM";

/* MQTT broker URI. Adjust as needed. */
#define MQTT_BROKER_URI "mqtt://192.168.0.1"
/* MQTT topic for mesh commands */
#define MQTT_TOPIC_COMMAND "mesh/command"

/* New MQTT client handle */
static esp_mqtt_client_handle_t mqtt_client = NULL;

/* Broadcast MAC address for ESP‑NOW (all 0xFF) */
static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* MQTT event handler */
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    // See esp-mqtt documentation for event types.
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC_COMMAND, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed to topic: %s", MQTT_TOPIC_COMMAND);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "Unsubscribed from topic");
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "Message published");
        break;
    case MQTT_EVENT_DATA: {
        ESP_LOGI(TAG, "MQTT data received. Topic: %.*s, Data: %.*s",
                 event->topic_len, event->topic,
                 event->data_len, event->data);
        /* If this node is the mesh root (e.g., level 0), broadcast the command */
        if (esp_mesh_lite_get_level() == 0) {
            ESP_LOGI(TAG, "Mesh root received MQTT command. Broadcasting to mesh.");
            /* Forward the received data over ESPNOW (using type ESPNOW_DATA_TYPE_RESERVE) */
            esp_err_t ret = espnow_send_wrapper(ESPNOW_DATA_TYPE_RESERVE,
                                                broadcast_mac,
                                                (const uint8_t *)event->data,
                                                event->data_len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to broadcast MQTT command: %s", esp_err_to_name(ret));
            }
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* MQTT event handler wrapper */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    mqtt_event_handler_cb(event_data);
}

void external_comm_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        // You can add username, password, etc., here if required.
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
    }
    
    /* Register the event handler */
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_LOGI(TAG, "External command interface initialized");
}

void external_comm_start(void)
{
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return;
    }
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT client started");
}