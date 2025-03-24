#ifndef CONSENSUS_H
#define CONSENSUS_H

#include <stdint.h>
#include <stdbool.h>
#include "blockchain.h"

// Initialize consensus by fetching the device's MAC.
void consensus_init(void);

// Returns true if this node is the leader based on MAC address.
bool consensus_am_i_leader(const uint8_t *leader_mac);

// Generate the Proof-of-Participation (PoP) for a block.
// Now accepts the leader's MAC address instead of a node ID.
void consensus_generate_pop_proof(block_t *block, const uint8_t *leader_mac);

// Verify that the block's PoP proof and sensor data are correct.
bool consensus_verify_block(block_t *block, sensor_record_t *my_sensor_data);

// Handle a dispute for a block.
void consensus_handle_dispute(uint32_t block_index, const uint8_t *src_mac);

#endif // CONSENSUS_H
