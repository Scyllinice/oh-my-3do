#include "ode_bus.h"
#include "sdk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static IOReq request;
static unsigned char status;
static long error, clock_ms;
static int busy;
Item FindAndOpenDevice(char *name) { (void)name; return 1; }
long CloseNamedDevice(Item item) { (void)item; return 0; }
Item GetTimerIOReq(void) { return 3; }
Item CreateIOReq(char *name, int pri, Item device, int port)
{ (void)name; (void)pri; (void)device; (void)port; return 2; }
void DeleteIOReq(Item item) { (void)item; }
long GetMSecTime(Item item) { (void)item; clock_ms += 1000; return clock_ms; }
Err SendIO(Item req, IOInfo *info)
{
    unsigned char *reply = (unsigned char *)info->ioi_Recv.iob_Buffer;
    const unsigned char *packet = (const unsigned char *)info->ioi_Send.iob_Buffer;
    (void)req;
    request.io_Error = 0; request.io_Actual = 2;
    if (error) return error;
    assert(packet && info->ioi_Send.iob_Len == 7);
    reply[0] = packet[0]; reply[1] = status; return 0;
}
int CheckIO(Item req) { (void)req; return !busy; }
Err WaitIO(Item req) { (void)req; return 0; }
Err AbortIO(Item req) { (void)req; return 0; }
void SleepMSec(Item req, long ms) { (void)req; (void)ms; }
void *LookupItem(Item req) { (void)req; return &request; }
int main(void)
{
    OdeBus bus;
    unsigned char packet[7] = {0xe4,0,0,0,0,0,0}, reply[32]; size_t len;
    assert(!ode_bus_open(&bus));
    status = 0x10;
    assert(!ode_bus_transfer(&bus, packet, reply, 32, &len));
    assert(bus.failed_command == 0xe4 && bus.failed_status == 0x10 && bus.commands == 1);
    status = 0; packet[0] = 0xe5;
    assert(!ode_bus_transfer(&bus, packet, reply, 32, &len));
    assert(bus.last_command == 0xe5 && bus.failed_command == 0xe4 && bus.last_length == 2);
    packet[0] = 0xc5; status = 0x10;
    assert(!ode_bus_transfer(&bus, packet, reply, 32, &len)); assert(bus.failed_command == 0xe4);
    packet[0] = 0xe3; error = -77;
    assert(ode_bus_transfer(&bus, packet, reply, 32, &len) < 0);
    assert(bus.failed_command == 0xe3 && bus.failure_error == -77 && !bus.last_length && !bus.failed_status);
    error = 0; status = 0; busy = 1; packet[0] = 0xe7;
    assert(ode_bus_transfer(&bus, packet, reply, 32, &len) < 0);
    assert(bus.timed_out && bus.failed_command == 0xe7 && bus.failure_error == -1);
    busy = 0;
    assert(ode_bus_transfer(&bus, packet, reply, 32, &len) < 0); assert(bus.commands == 5);
    ode_bus_close(&bus);
    puts("Diagnostics: actual bus capture, retained failures, launch status and timeout gating passed");
    return 0;
}
