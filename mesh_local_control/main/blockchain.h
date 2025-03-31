#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_mesh_lite.h"

#define MAX_NODES       3   // Maximum number of sensor records per block
#define MAX_NEIGHBORS   5   // Maximum number of neighbor RSSI readings per sensor record
#define HEATMAP_SIZE    3   // Dummy size for heatmap data

// Structure for a sensor record.
typedef struct sensor_record {
    uint8_t mac[ESP_NOW_ETH_ALEN];     // Device MAC address
    uint32_t timestamp;                // Timestamp (in seconds)
    float temperature;                 // Temperature in °C
    float humidity;                    // Humidity in %
    int8_t rssi[MAX_NEIGHBORS];        // Array of RSSI values from neighbors
    struct sensor_record *next;        // Linked-list pointer
} sensor_record_t;

// Structure for a blockchain block.
typedef struct {
    uint32_t timestamp;                // Block creation time (in seconds)
    uint8_t prev_hash[32];             // Previous block hash
    uint32_t num_sensor_readings;      // Number of sensor records in the block
    sensor_record_t *node_data;        // Head pointer to the linked list of sensor records
    uint8_t heatmap[HEATMAP_SIZE];     // Dummy heatmap data
    uint8_t hash[32];                  // Block’s hash (computed from contents)
    char pop_proof[64];                // Proof-of-Participation string
} block_t;

// Public blockchain API.
uint32_t blockchain_init(void);
void blockchain_deinit(void);
void blockchain_create_block(block_t *new_block, sensor_record_t sensor_data[MAX_NODES]);
bool blockchain_add_block(block_t *new_block);
bool blockchain_get_last_block(block_t *block_out);
void blockchain_print_last_block(void);
void blockchain_print_history(void);  // New function to print the full blockchain history.
void blockchain_receive_block(const uint8_t *data, uint16_t len);
void sensor_blockchain_task(void *pvParameters);
void calculate_block_hash(block_t *block);

// Helper: size of a sensor record (excluding the pointer)
static const size_t sensor_size = sizeof(uint8_t)*ESP_NOW_ETH_ALEN + sizeof(uint32_t) + sizeof(float)*2 + (MAX_NEIGHBORS*sizeof(int8_t));

#endif // BLOCKCHAIN_H
