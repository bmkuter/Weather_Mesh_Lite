#ifndef NODE_ID_H
#define NODE_ID_H

#include <stdint.h>

// Derive a 16-bit node ID from the device's MAC address.
uint16_t get_my_node_id(void);

#endif // NODE_ID_H
