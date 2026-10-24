#include "ode_bus.h"

#include "device.h"
#include "io.h"
#include "item.h"
#include "timerutils.h"
#include <string.h>

/* Internal MEI-compatible ODEs, including FCare/SataTo3DO, are on unit 0.
 * Override only for a known compatible device on another XBUS unit.
 */
#ifndef ODE_XBUS_UNIT
#define ODE_XBUS_UNIT 0
#endif

void ode_bus_close(OdeBus *bus)
{
    if (bus->data_request > 0) DeleteIOReq(bus->data_request);
    if (bus->request > 0) DeleteIOReq(bus->request);
    if (bus->timer > 0) DeleteIOReq(bus->timer);
    if (bus->device > 0) CloseNamedDevice(bus->device);
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

static int bus_failure(OdeBus *bus, Err error)
{
    bus->last_error = error;
    bus->failed_command = bus->last_command;
    bus->failed_status = bus->last_length ? bus->last_reply[bus->last_length - 1] : 0;
    bus->failure_error = error;
    return -1;
}

int ode_bus_transfer(void *context, const unsigned char *command,
                     unsigned char *reply, size_t capacity, size_t *length)
{
    OdeBus *bus = (OdeBus *)context;
    IOInfo info;
    IOReq *request;
    Err error;
    unsigned long started;
    unsigned int spins = 0;
    if (bus->timed_out || bus->request < 0) return -1;
    bus->last_error = 0;
    bus->last_command = command[0];
    bus->last_length = 0;
    memset(bus->last_reply, 0, sizeof(bus->last_reply));
    ++bus->commands;
    memset(&info, 0, sizeof(info));
    memset(reply, 0, capacity);
    *length = 0;
    info.ioi_Command = 3; /* Portfolio XBUSCMD_Command */
    info.ioi_Unit = ODE_XBUS_UNIT;
    info.ioi_Send.iob_Buffer = (void *)command;
    info.ioi_Send.iob_Len = 7;
    info.ioi_Recv.iob_Buffer = reply;
    info.ioi_Recv.iob_Len = (s32)capacity;
    started = (unsigned long)GetMSecTime(bus->timer);
    error = SendIO(bus->request, &info);
    if (error < 0) {
        return bus_failure(bus, error);
    }
    while (!bus->blocking && !CheckIO(bus->request)) {
        /* Most status replies arrive quickly. Yield on slower media and
         * bound every request so an absent ODE cannot freeze the menu. */
        if (++spins >= 128) {
            if ((unsigned long)GetMSecTime(bus->timer) - started >= 3000UL) {
                AbortIO(bus->request);
                WaitIO(bus->request);
                bus->timed_out = 1;
                /* Aborting cannot prove the hardware FIFO is synchronized.
                 * Do not issue further commands until a console restart. */
                return bus_failure(bus, -1);
            }
            SleepMSec(bus->timer, 1);
            spins = 0;
        }
    }
    error = WaitIO(bus->request);
    request = (IOReq *)LookupItem(bus->request);
    if (error < 0 || !request || request->io_Error < 0) {
        return bus_failure(bus, error < 0 ? error : (request ? request->io_Error : -1));
    }
    if (request->io_Actual < 0 || (size_t)request->io_Actual > capacity || request->io_Actual > 32)
        return bus_failure(bus, -1);
    *length = (size_t)request->io_Actual;
    bus->last_length = *length;
    memcpy(bus->last_reply, reply, *length);
    if (*length && command[0] != 0xc5 && (reply[*length - 1] & 0x10)) {
        /* Keep the rejected command even if a later close/navigation succeeds. */
        bus_failure(bus, 0);
    }
    return 0;
}

int ode_bus_read_buffer(void *context, unsigned char *data, size_t size)
{
    OdeBus *bus = (OdeBus *)context;
    static const unsigned char packet[7] = {0xe7, 0, 0, 0, 0, 0, 0};
    unsigned char reply[32];
    size_t length = 0;
    IOInfo info;
    IOReq *request;
    Item data_request;
    unsigned long started;
    Err error;
    int result = -1;
    if (bus->timed_out || bus->device < 0 || size != 2048) return -1;
    if (bus->data_request <= 0)
        bus->data_request = CreateIOReq("Oh My 3DO USB data", 0, bus->device, 0);
    data_request = bus->data_request;
    if (data_request < 0) { bus->last_error = data_request; return -1; }
    memset(&info, 0, sizeof(info));
    info.ioi_Command = CMD_READ;
    info.ioi_Unit = ODE_XBUS_UNIT;
    info.ioi_Recv.iob_Buffer = data;
    info.ioi_Recv.iob_Len = (s32)size;
    /* E7 streams data before its status. Arm the separate data FIFO request
     * first so the ODE cannot stall waiting for the console to receive it. */
    error = SendIO(data_request, &info);
    if (error < 0) {
        bus->last_error = error;
        return -1;
    }
    if (ode_bus_transfer(context, packet, reply, sizeof(reply), &length) < 0 ||
        length != 2 || reply[0] != 0xe7 || (reply[1] & 0x10)) goto done;
    started = (unsigned long)GetMSecTime(bus->timer);
    while (!bus->blocking && !CheckIO(data_request)) {
        if ((unsigned long)GetMSecTime(bus->timer) - started >= 3000UL) goto done;
        SleepMSec(bus->timer, 1);
    }
    error = WaitIO(data_request);
    request = (IOReq *)LookupItem(data_request);
    if (error < 0 || !request || request->io_Error < 0) {
        bus->last_error = error < 0 ? error : (request ? request->io_Error : -1);
        goto done;
    }
    if ((size_t)request->io_Actual != size) goto done;
    result = 0;
done:
    if (result < 0) {
        AbortIO(data_request);
        WaitIO(data_request);
        /* A partial two-FIFO exchange cannot safely be retried. */
        bus->timed_out = 1;
        bus_failure(bus, bus->last_error < 0 ? bus->last_error : -1);
    }
    /* Completed requests can be submitted again. Keep this separate from the
     * command request so E7 still arms data before waiting for status. Failed
     * two-FIFO exchanges stay blocked until ode_bus_close/open. */
    return result;
}
