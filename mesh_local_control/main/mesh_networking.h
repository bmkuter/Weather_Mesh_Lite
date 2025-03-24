#ifndef MESH_NETWORKING_H
#define MESH_NETWORKING_H

#include "my_includes.h"
#include "blockchain.h"
#include "temperature_probe.h"
#include "node_id.h"
#include "consensus.h"

void espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len);
void add_self_broadcast_peer(void);
void espnow_periodic_send_task(void *arg);
esp_err_t espnow_send_wrapper(uint8_t type, const uint8_t *dest_addr, const uint8_t *data, size_t len);

#endif
