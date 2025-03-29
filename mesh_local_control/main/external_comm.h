#ifndef EXTERNAL_COMM_H
#define EXTERNAL_COMM_H

/**
 * @brief Initialize the MQTT external command interface.
 */
void external_comm_init(void);

/**
 * @brief Start the MQTT client process.
 */
void external_comm_start(void);

#endif // EXTERNAL_COMM_H