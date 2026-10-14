#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../Drivers/API/Src/examples/ex_02a_simple_rx/rx_rssi_stream.h"

static uint8_t output[4096];
static unsigned output_size;
static int full;
void SEGGER_RTT_Init(void) { assert(!"Do not reset existing terminal channel"); }
int SEGGER_RTT_ConfigUpBuffer(unsigned index, const char *name, void *buffer,
                              unsigned size, unsigned flags)
{
    assert(index == 1 && strcmp(name, "RSSI_ALL_V1") == 0);
    assert(buffer && size == RX_RSSI_STREAM_BYTES && flags == SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    return 0;
}
unsigned SEGGER_RTT_Write(unsigned index, const void *buffer, unsigned size)
{
    assert(index == 1 && size == RX_RSSI_WIRE_BYTES);
    if (full) { return 0; }
    assert(output_size + size <= sizeof(output));
    memcpy(output + output_size, buffer, size);
    output_size += size;
    return size;
}
static uint32_t word(unsigned offset)
{
    return (uint32_t)output[offset] | ((uint32_t)output[offset+1] << 8) |
           ((uint32_t)output[offset+2] << 16) | ((uint32_t)output[offset+3] << 24);
}
int main(void)
{
    rx_rssi_stream_init(300000);
    assert(output_size == 16 && output[0] == 'R' && output[1] == 'S');
    assert(word(4) == 300000 && word(8) == 1 && word(12) == 16);
    rx_rssi_stream_record(7, 11, -20672, -21000, 0);
    rx_rssi_stream_record(7, 12, INT16_MIN, INT16_MIN, 1);
    assert(output[18] == 1 && output[19] == 3 && word(20) == 7 && word(24) == 11);
    assert((int16_t)(word(28) & 0xffff) == -20672);
    assert(output[35] == 4 && word(40) == 12);
    /* A full data channel never blocks RX; the final footer proves incompleteness. */
    full = 1;
    rx_rssi_stream_record(8, 13, -12345, -15000, 0);
    assert(rx_rssi_records == 3 && rx_rssi_dropped == 1 && output_size == 48);
    rx_rssi_stream_finish(2);
    rx_rssi_stream_flush();
    assert(rx_rssi_end_pending && output_size == 48);
    full = 0;
    rx_rssi_stream_flush();
    assert(!rx_rssi_end_pending && output_size == 64);
    assert(output[50] == 2 && word(52) == 2 && word(56) == 3 && word(60) == 1);
    rx_rssi_stream_flush();
    assert(output_size == 64);
    puts("PASS: independent binary channel, sequence/ID/Q8 pairing, duplicate/invalid flags, counted nonblocking overflow, retried END");
    return 0;
}
