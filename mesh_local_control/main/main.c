#include "my_includes.h"
#include "logger.h"
#include "mesh_networking.h"
#include "wifi_networking.h"
#include "my_utility.h"
#include "blockchain.h"
#include "temperature_probe.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "node_response.h"
#include "election_response.h"
#include "external_comm.h"
#include "ws_comm.h"
#include "secrets.h" // Include your secrets header for SSID and password

#define PAYLOAD_LEN       (1456) /**< Max payload size(in bytes) */
#define UART_PORT_NUM       UART_NUM_1
#define UART_BAUD_RATE      115200
#define UART_TX_GPIO        21
#define UART_RX_GPIO        20

#define I2C_MASTER_PORT_NUM      I2C_NUM_0
#define I2C_MASTER_SCL_IO        40      // Example GPIO number for SCL
#define I2C_MASTER_SDA_IO        41      // Example GPIO number for SDA
#define I2C_MASTER_FREQ_HZ       100000

static const char *TAG = "main";

static void i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_PORT_NUM, &conf);
    esp_err_t ret = i2c_driver_install(I2C_MASTER_PORT_NUM, conf.mode, 0, 0, 0);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed");
    }
}

void app_main()
{
    /**
     * @brief Set the log level for serial port printing.
     */
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_storage_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_bridge_create_all_netif();

    wifi_init();

    esp_mesh_lite_config_t mesh_lite_config = ESP_MESH_LITE_DEFAULT_INIT();
    mesh_lite_config.join_mesh_ignore_router_status = true;
    mesh_lite_config.join_mesh_without_configured_wifi = true;
    esp_mesh_lite_init(&mesh_lite_config);

    app_wifi_set_softap_info();

    esp_mesh_lite_start();

    // /* Initialize and start external MQTT interface */
    // external_comm_init();
    // external_comm_start();
    // ws_comm_init();
    // ws_comm_start();

    /**
     * @breif Create handler
     */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_sta_got_ip_handler, NULL, NULL));

    TimerHandle_t timer = xTimerCreate("print_system_info", 10000 / portTICK_PERIOD_MS,
                                       true, NULL, print_system_info_timercb);
    xTimerStart(timer, 0);

    // Register ESPNOW receive callback
    esp_mesh_lite_espnow_recv_cb_register(ESPNOW_DATA_TYPE_RESERVE, espnow_recv_cb);
    add_self_broadcast_peer();

    // xTaskCreate(espnow_periodic_send_task, "espnow_periodic_send_task", 4096, NULL, 5, NULL);

    // ESP_LOGW(TAG, "-----");
    // esp_mesh_lite_report_info();
    // ESP_LOGW(TAG, "-----");

    i2c_master_init();
    temperature_probe_init();
    node_response_init();
    election_response_init();

    vTaskDelay(3000/portTICK_PERIOD_MS);    

    xTaskCreate(sensor_blockchain_task, "sensor_blockchain_task", 4096 * 2, NULL, 5, NULL);
}