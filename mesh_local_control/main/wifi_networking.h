#ifndef WIFI_NETWORKING_H
#define WIFI_NETWORKING_H

#include "my_includes.h"

int socket_tcp_client_create(const char *ip, uint16_t port);
void tcp_client_write_task(void *arg);
void ip_event_sta_got_ip_handler(void *arg, esp_event_base_t event_base,
    int32_t event_id, void *event_data);
void wifi_init(void);
void app_wifi_set_softap_info(void);
void tcp_server_task(void *arg);

#endif
