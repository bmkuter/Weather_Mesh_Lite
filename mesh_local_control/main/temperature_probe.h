#ifndef TEMPERATURE_PROBE_H
#define TEMPERATURE_PROBE_H

#include "my_includes.h"

void temperature_probe_init();
float temperature_probe_read_temperature();
float temperature_probe_read_humidity();
void temperature_task(void *arg);

#endif // TEMPERATURE_PROBE_H
