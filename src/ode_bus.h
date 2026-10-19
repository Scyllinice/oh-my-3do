#ifndef OH_MY_3DO_ODE_BUS_H
#define OH_MY_3DO_ODE_BUS_H

#include <stddef.h>
#include "types.h"

typedef struct OdeBus {
    Item device;
    Item request;
    Item data_request;
    Item timer;
    int timed_out;
    Err last_error;
    unsigned char last_command;
    unsigned char last_reply[32];
    size_t last_length;
    unsigned long commands;
    unsigned char failed_command;
    unsigned char failed_status;
    Err failure_error;
} OdeBus;

int ode_bus_open(OdeBus *bus);
void ode_bus_close(OdeBus *bus);
int ode_bus_transfer(void *context, const unsigned char *command,
                     unsigned char *reply, size_t capacity, size_t *length);
int ode_bus_read_buffer(void *context, unsigned char *data, size_t size);

#endif
