#ifndef CONSENSUS_H
#define CONSENSUS_H

#include <stdint.h>
#include <stdbool.h>
#include "blockchain.h"

// Initialize consensus with this node's ID.
void consensus_init(uint16_t my_node_id);

// Returns true if this node is the leader for the given block ID.
bool consensus_am_i_leader(uint32_t block_id);

// Generate the Proof-of-Participation (PoP) for a block.
void consensus_generate_pop_proof(block_t *block, uint16_t leader_node_id);

// Verify that the block's PoP proof and sensor data are correct.
bool consensus_verify_block(block_t *block, sensor_record_t *my_sensor_data);

// Handle a dispute for a block.
void consensus_handle_dispute(uint32_t block_index, const uint8_t *src_mac);

#endif // CONSENSUS_H
