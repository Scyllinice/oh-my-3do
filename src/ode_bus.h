/*
 * The console side of the ODE connection: sending commands to the drive
 * emulator through the 3DO's expansion bus (XBUS), where the CD drive sits.
 *
 * This provides the OdeTransfer (ode_bus_transfer) and OdeReadBuffer
 * (ode_bus_read_buffer) functions the rest of the menu uses, on real
 * hardware. Tests use stand-ins instead. Every request has a time limit, so
 * a missing or stuck drive can never freeze the menu.
 */
#ifndef OH_MY_3DO_ODE_BUS_H
#define OH_MY_3DO_ODE_BUS_H

#include <stddef.h>
#include "types.h"

/* An open connection, plus what is kept for the diagnostics screen. */
typedef struct OdeBus {
    Item device;       /* the XBUS device */
    Item request;      /* I/O request for commands and their status replies */
    Item data_request; /* I/O request for the data channel (see ode_bus_read_buffer) */
    Item timer;        /* for timing and short sleeps */

    /*
     * Set when a request ran out of time or a two-part exchange was cut
     * short. The command channel may then be out of step with the drive,
     * so nothing more is sent until the console restarts.
     */
    int timed_out;

    Err last_error;
    unsigned char last_command;
    unsigned char last_reply[32];
    size_t last_length;
    unsigned long commands; /* commands sent so far */

    /* The last command the drive rejected or that failed, kept even if
     * later commands succeed. */
    unsigned char failed_command;
    unsigned char failed_status;
    Err failure_error;

    /* Wait for each request's completion signal instead of polling it.
     * Only the stream bench's reader thread sets this. */
    int blocking;

    /* Count and total time per kind of command (see ode_bus_kind). */
    unsigned long kind_count[17], kind_ms[17];
} OdeBus;

/* Groups commands for timing: 0-7 for E0-E7, 8-15 for C0-C7, 16 for others. */
int ode_bus_kind(unsigned char command);

/* Opens the XBUS device and the requests it needs. Returns 0, or -1. */
int ode_bus_open(OdeBus *bus);

/* Closes everything ode_bus_open opened. */
void ode_bus_close(OdeBus *bus);

/* Sends one 7-byte command and waits for its status reply (an OdeTransfer). */
int ode_bus_transfer(void *context, const unsigned char *command, unsigned char *reply,
                     size_t capacity, size_t *length);

/* Copies the drive's 2048-byte data buffer to `data` (an OdeReadBuffer;
 * `size` must be 2048). Returns 0, or -1. */
int ode_bus_read_buffer(void *context, unsigned char *data, size_t size);

#endif
