#include <inttypes.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_wifi.h"
#include "nvs_flash.h"
#include <sys/socket.h>

#include "esp_bridge.h"
#include "esp_mesh_lite.h"

#include "driver/uart.h"
#include "esp_now.h"