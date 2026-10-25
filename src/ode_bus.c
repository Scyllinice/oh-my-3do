/*
 * Sending ODE commands over the 3DO's expansion bus. See ode_bus.h.
 *
 * Portfolio (the 3DO's operating system) handles the bus as a device. Each
 * exchange is an I/O request: SendIO starts it, CheckIO says whether it has
 * finished, WaitIO waits for it, and AbortIO cancels it.
 */
#include "ode_bus.h"

#include "device.h"
#include "io.h"
#include "item.h"
#include "timerutils.h"
#include <string.h>

/*
 * The XBUS unit the drive is on. Internal MEI-compatible drive emulators,
 * the FCare (SataTo3DO) included, are unit 0. Only change it for a known
 * compatible device on another unit.
 */
#ifndef ODE_XBUS_UNIT
#define ODE_XBUS_UNIT 0
#endif

/* Portfolio's XBUS command for "send a command and receive its status". */
#define XBUSCMD_COMMAND 3

/* The data-buffer command, and the data buffer's size. */
#define CMD_READ_BUFFER 0xe7
#define BUFFER_BYTES    2048

/* Starting the disc queue sets the error status bit on purpose (see ode.c). */
#define CMD_START_QUEUE 0xc5
#define STATUS_ERROR    0x10

/* How long any one request may take, in milliseconds. */
#define REQUEST_TIMEOUT_MS 3000UL

/* How many quick checks to make before starting to sleep between them. */
#define QUICK_CHECKS 128

void ode_bus_close(OdeBus *bus)
{
    if (bus->data_request > 0)
        DeleteIOReq(bus->data_request);
    if (bus->request > 0)
        DeleteIOReq(bus->request);
    if (bus->timer > 0)
        DeleteIOReq(bus->timer);
    if (bus->device > 0)
        CloseNamedDevice(bus->device);
    bus->data_request = bus->request = bus->timer = bus->device = -1;
}

int ode_bus_open(OdeBus *bus)
{
    memset(bus, 0, sizeof(*bus));
    bus->data_request = bus->request = bus->timer = bus->device = -1;

    bus->device = FindAndOpenDevice("xbus");
    if (bus->device < 0) {
        bus->last_error = bus->device;
        return -1;
    }
    bus->request = CreateIOReq("Oh My 3DO ODE", 0, bus->device, 0);
    bus->timer = GetTimerIOReq();
    if (bus->request < 0 || bus->timer < 0) {
        bus->last_error = bus->request < 0 ? bus->request : bus->timer;
        ode_bus_close(bus);
        return -1;
    }
    return 0;
}

/* Records a failure for diagnostics and returns -1. */
static int bus_failure(OdeBus *bus, Err error)
{
    bus->last_error = error;
    bus->failed_command = bus->last_command;
    bus->failed_status = bus->last_length ? bus->last_reply[bus->last_length - 1] : 0;
    bus->failure_error = error;
    return -1;
}

int ode_bus_kind(unsigned char command)
{
    if ((command & 0xf8) == 0xe0)
        return command & 7;
    if ((command & 0xf8) == 0xc0)
        return 8 + (command & 7);
    return 16;
}

/* Milliseconds since `started`, on the bus's timer. */
static unsigned long elapsed_ms(OdeBus *bus, unsigned long started)
{
    return (unsigned long)GetMSecTime(bus->timer) - started;
}

int ode_bus_transfer(void *context, const unsigned char *command, unsigned char *reply,
                     size_t capacity, size_t *length)
{
    OdeBus *bus = (OdeBus *)context;
    IOInfo info;
    IOReq *finished;
    Err error;
    unsigned long started;
    unsigned int checks = 0;
    int kind = ode_bus_kind(command[0]);

    if (bus->timed_out || bus->request < 0)
        return -1;

    bus->last_error = 0;
    bus->last_command = command[0];
    bus->last_length = 0;
    memset(bus->last_reply, 0, sizeof(bus->last_reply));
    ++bus->commands;
    memset(reply, 0, capacity);
    *length = 0;

    /* Send the 7 command bytes; receive up to `capacity` status bytes. */
    memset(&info, 0, sizeof(info));
    info.ioi_Command = XBUSCMD_COMMAND;
    info.ioi_Unit = ODE_XBUS_UNIT;
    info.ioi_Send.iob_Buffer = (void *)command;
    info.ioi_Send.iob_Len = 7;
    info.ioi_Recv.iob_Buffer = reply;
    info.ioi_Recv.iob_Len = (s32)capacity;
    started = (unsigned long)GetMSecTime(bus->timer);
    error = SendIO(bus->request, &info);
    if (error < 0)
        return bus_failure(bus, error);

    /*
     * Wait for the reply. Most arrive almost at once, so check repeatedly
     * first; after that, sleep a millisecond between checks so slower media
     * do not hog the processor, and give up after the time limit.
     */
    while (!bus->blocking && !CheckIO(bus->request)) {
        if (++checks >= QUICK_CHECKS) {
            if (elapsed_ms(bus, started) >= REQUEST_TIMEOUT_MS) {
                AbortIO(bus->request);
                WaitIO(bus->request);
                /*
                 * Cancelling cannot prove the drive and console agree on
                 * where the command channel is. Send nothing more until the
                 * console restarts.
                 */
                bus->timed_out = 1;
                return bus_failure(bus, -1);
            }
            SleepMSec(bus->timer, 1);
            checks = 0;
        }
    }

    error = WaitIO(bus->request);
    finished = (IOReq *)LookupItem(bus->request);
    if (error < 0)
        return bus_failure(bus, error);
    if (!finished)
        return bus_failure(bus, -1);
    if (finished->io_Error < 0)
        return bus_failure(bus, finished->io_Error);
    if (finished->io_Actual < 0 || (size_t)finished->io_Actual > capacity ||
        finished->io_Actual > 32)
        return bus_failure(bus, -1);

    *length = (size_t)finished->io_Actual;
    ++bus->kind_count[kind];
    bus->kind_ms[kind] += elapsed_ms(bus, started);
    bus->last_length = *length;
    memcpy(bus->last_reply, reply, *length);

    /* Note a rejected command for diagnostics, even if later ones succeed.
     * (Whether it counts as a failure is up to the caller.) */
    if (*length && command[0] != CMD_START_QUEUE && (reply[*length - 1] & STATUS_ERROR))
        bus_failure(bus, 0);
    return 0;
}

int ode_bus_read_buffer(void *context, unsigned char *data, size_t size)
{
    static const unsigned char read_command[7] = {CMD_READ_BUFFER, 0, 0, 0, 0, 0, 0};
    OdeBus *bus = (OdeBus *)context;
    unsigned char reply[32];
    size_t length = 0;
    IOInfo info;
    IOReq *finished;
    Item data_request;
    unsigned long started;
    Err error;
    int result = -1;

    if (bus->timed_out || bus->device < 0 || size != BUFFER_BYTES)
        return -1;

    /* The data channel has its own request, made once and reused. */
    if (bus->data_request <= 0)
        bus->data_request = CreateIOReq("Oh My 3DO USB data", 0, bus->device, 0);
    data_request = bus->data_request;
    if (data_request < 0) {
        bus->last_error = data_request;
        return -1;
    }

    /*
     * The E7 command sends its 2048 bytes on the data channel before its
     * status reply. So start receiving on the data channel first: otherwise
     * the drive would wait for the console to take the data while the
     * console waited for the status, and neither would move.
     */
    memset(&info, 0, sizeof(info));
    info.ioi_Command = CMD_READ;
    info.ioi_Unit = ODE_XBUS_UNIT;
    info.ioi_Recv.iob_Buffer = data;
    info.ioi_Recv.iob_Len = (s32)size;
    error = SendIO(data_request, &info);
    if (error < 0) {
        bus->last_error = error;
        return -1;
    }

    /* Then send the command and check its status. */
    if (ode_bus_transfer(context, read_command, reply, sizeof(reply), &length) < 0 || length != 2 ||
        reply[0] != CMD_READ_BUFFER || (reply[1] & STATUS_ERROR))
        goto done;

    /* Then wait for the data to have arrived. */
    started = (unsigned long)GetMSecTime(bus->timer);
    while (!bus->blocking && !CheckIO(data_request)) {
        if (elapsed_ms(bus, started) >= REQUEST_TIMEOUT_MS)
            goto done;
        SleepMSec(bus->timer, 1);
    }
    error = WaitIO(data_request);
    finished = (IOReq *)LookupItem(data_request);
    if (error < 0 || !finished || finished->io_Error < 0) {
        bus->last_error = error < 0 ? error : (finished ? finished->io_Error : -1);
        goto done;
    }
    if ((size_t)finished->io_Actual != size)
        goto done;
    result = 0;

done:
    if (result < 0) {
        AbortIO(data_request);
        WaitIO(data_request);
        /* Half of a two-channel exchange cannot safely be repeated: stop. */
        bus->timed_out = 1;
        bus_failure(bus, bus->last_error < 0 ? bus->last_error : -1);
    }
    /*
     * A finished request can be sent again, so the data request is kept for
     * the next read. It stays separate from the command request so the data
     * channel can still be started before the command.
     */
    return result;
}
