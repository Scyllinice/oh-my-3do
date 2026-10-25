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
    /* Wait for each request's completion signal instead of polling it.
     * Only the stream bench's reader thread sets this. */
    int blocking;
    /* Time per command kind for diagnostics: E0-E7, then C0-C7, then others. */
    unsigned long kind_count[17], kind_ms[17];
} OdeBus;
int ode_bus_kind(unsigned char command);

int ode_bus_open(OdeBus *bus);
void ode_bus_close(OdeBus *bus);
int ode_bus_transfer(void *context, const unsigned char *command, unsigned char *reply,
                     size_t capacity, size_t *length);
int ode_bus_read_buffer(void *context, unsigned char *data, size_t size);

#endif
