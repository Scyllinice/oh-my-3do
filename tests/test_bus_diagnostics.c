#include "ode_bus.h"
#include "sdk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static IOReq requests[32];
static unsigned char status;
static long error, clock_ms;
static int busy;
static int created, deleted, aborted, data_reads, short_read;
static int verify_order;
static Item armed_data;
Item FindAndOpenDevice(char *name)
{
    (void)name;
    return 1;
}
long CloseNamedDevice(Item item)
{
    (void)item;
    return 0;
}
Item GetTimerIOReq(void)
{
    return 3;
}
Item CreateIOReq(char *name, int pri, Item device, int port)
{
    (void)name;
    (void)pri;
    (void)device;
    (void)port;
    return 3 + (++created);
}
void DeleteIOReq(Item item)
{
    (void)item;
    ++deleted;
}
long GetMSecTime(Item item)
{
    (void)item;
    clock_ms += 1000;
    return clock_ms;
}
Err SendIO(Item req, IOInfo *info)
{
    unsigned char *reply = (unsigned char *)info->ioi_Recv.iob_Buffer;
    const unsigned char *packet = (const unsigned char *)info->ioi_Send.iob_Buffer;
    requests[req].io_Error = 0;
    requests[req].io_Actual = 2;
    if (error)
        return error;
    if (info->ioi_Command == CMD_READ) {
        assert(!packet && info->ioi_Recv.iob_Len == 2048 && !armed_data);
        armed_data = req;
        memset(reply, 0x5a, 2048);
        requests[req].io_Actual = short_read ? 2047 : 2048;
        ++data_reads;
        return 0;
    }
    assert(packet && info->ioi_Send.iob_Len == 7);
    if (verify_order && packet[0] == 0xe7)
        assert(armed_data && req != armed_data);
    reply[0] = packet[0];
    reply[1] = status;
    return 0;
}
int CheckIO(Item req)
{
    (void)req;
    return !busy;
}
Err WaitIO(Item req)
{
    if (req == armed_data)
        armed_data = 0;
    return 0;
}
Err AbortIO(Item req)
{
    (void)req;
    ++aborted;
    return 0;
}
void SleepMSec(Item req, long ms)
{
    (void)req;
    (void)ms;
}
void *LookupItem(Item req)
{
    return &requests[req];
}
int main(void)
{
    OdeBus bus;
    unsigned char packet[7] = {0xe4, 0, 0, 0, 0, 0, 0}, reply[32];
    size_t len;
    assert(!ode_bus_open(&bus));
    status = 0x10;
    assert(!ode_bus_transfer(&bus, packet, reply, 32, &len));
    assert(bus.failed_command == 0xe4 && bus.failed_status == 0x10 && bus.commands == 1);
    status = 0;
    packet[0] = 0xe5;
    assert(!ode_bus_transfer(&bus, packet, reply, 32, &len));
    assert(bus.last_command == 0xe5 && bus.failed_command == 0xe4 && bus.last_length == 2);
    packet[0] = 0xc5;
    status = 0x10;
    assert(!ode_bus_transfer(&bus, packet, reply, 32, &len));
    assert(bus.failed_command == 0xe4);
    packet[0] = 0xe3;
    error = -77;
    assert(ode_bus_transfer(&bus, packet, reply, 32, &len) < 0);
    assert(bus.failed_command == 0xe3 && bus.failure_error == -77 && !bus.last_length &&
           !bus.failed_status);
    error = 0;
    status = 0;
    busy = 1;
    packet[0] = 0xe7;
    assert(ode_bus_transfer(&bus, packet, reply, 32, &len) < 0);
    assert(bus.timed_out && bus.failed_command == 0xe7 && bus.failure_error == -1);
    busy = 0;
    assert(ode_bus_transfer(&bus, packet, reply, 32, &len) < 0);
    assert(bus.commands == 5);
    ode_bus_close(&bus);
    error = 0;
    status = 0;
    busy = 0;
    {
        unsigned char block[2048];
        int before_creates = created, before_deletes = deleted, before_aborts = aborted;
        Item data_request;
        verify_order = 1;
        assert(!ode_bus_open(&bus) && bus.data_request < 0);
        assert(!ode_bus_read_buffer(&bus, block, sizeof(block)));
        assert(block[0] == 0x5a && block[2047] == 0x5a);
        data_request = bus.data_request;
        assert(!ode_bus_read_buffer(&bus, block, sizeof(block)));
        assert(bus.data_request == data_request && created == before_creates + 2 &&
               deleted == before_deletes && data_reads == 2);
        short_read = 1;
        assert(ode_bus_read_buffer(&bus, block, sizeof(block)) < 0 && bus.timed_out);
        assert(aborted == before_aborts + 1 && created == before_creates + 2);
        assert(ode_bus_read_buffer(&bus, block, sizeof(block)) < 0 && data_reads == 3);
        ode_bus_close(&bus);
        assert(deleted == before_deletes + 3 && bus.data_request < 0);
        ode_bus_close(&bus);
        assert(deleted == before_deletes + 3);
    }
    puts("Diagnostics: actual bus capture, retained failures, launch status and timeout gating "
         "passed");
    return 0;
}
