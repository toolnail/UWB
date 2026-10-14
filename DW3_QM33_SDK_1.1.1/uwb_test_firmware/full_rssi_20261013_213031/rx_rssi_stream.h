/* Channel 0 remains the existing text log. Channel 1 is a nonblocking binary
 * archive, written only by main. A full channel is counted, never stalls RX. */
#ifndef RX_RSSI_STREAM_H
#define RX_RSSI_STREAM_H

#include <stdint.h>
#include <string.h>
#include "SEGGER/SEGGER_RTT.h"

#define RX_RSSI_STREAM_BYTES 32768U
#define RX_RSSI_WIRE_BYTES 16U
#define RX_RSSI_FLAG_RSSI 1U
#define RX_RSSI_FLAG_FP 2U
#define RX_RSSI_FLAG_DUPLICATE 4U
static uint8_t rx_rssi_buffer[RX_RSSI_STREAM_BYTES];
static uint32_t rx_rssi_records;
static uint32_t rx_rssi_dropped;
static uint8_t rx_rssi_end[RX_RSSI_WIRE_BYTES];
static uint8_t rx_rssi_end_pending;

static void rx_rssi_put32(uint8_t *out, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; i++) { out[i] = (uint8_t)(value >> (8U * i)); }
}

/* Little-endian: magic[2], type, flags, sequence/u32, reception/u32,
 * RSSI/q8.8/i16, FP/q8.8/i16. START/END reuse the final four bytes as u32. */
static void rx_rssi_wire(uint8_t *out, uint8_t type, uint8_t flags,
                         uint32_t sequence, uint32_t reception, uint32_t values)
{
    out[0] = 'R'; out[1] = 'S'; out[2] = type; out[3] = flags;
    rx_rssi_put32(out + 4, sequence);
    rx_rssi_put32(out + 8, reception);
    rx_rssi_put32(out + 12, values);
}

static void rx_rssi_stream_init(uint32_t target)
{
    uint8_t start[RX_RSSI_WIRE_BYTES];
    rx_rssi_records = rx_rssi_dropped = 0U;
    rx_rssi_end_pending = 0U;
    /* ConfigUpBuffer initializes RTT if needed. Do not call RTT_Init here:
     * that would erase channel 0's already-written application banner. */
    if (SEGGER_RTT_ConfigUpBuffer(1U, "RSSI_ALL_V1", rx_rssi_buffer,
                                sizeof(rx_rssi_buffer), SEGGER_RTT_MODE_NO_BLOCK_SKIP) < 0)
    {
        rx_rssi_dropped++;
    }
    rx_rssi_wire(start, 0U, 0U, target, 1U, RX_RSSI_WIRE_BYTES);
    if (SEGGER_RTT_Write(1U, start, sizeof(start)) != sizeof(start)) { rx_rssi_dropped++; }
}

static void rx_rssi_stream_record(uint32_t sequence, uint32_t reception,
                                  int16_t rssi, int16_t fp, int duplicate)
{
    uint8_t wire[RX_RSSI_WIRE_BYTES];
    uint8_t flags = (rssi != INT16_MIN ? RX_RSSI_FLAG_RSSI : 0U) |
                    (fp != INT16_MIN ? RX_RSSI_FLAG_FP : 0U) |
                    (duplicate ? RX_RSSI_FLAG_DUPLICATE : 0U);
    uint32_t values = (uint32_t)(uint16_t)rssi | ((uint32_t)(uint16_t)fp << 16U);
    rx_rssi_wire(wire, 1U, flags, sequence, reception, values);
    rx_rssi_records++;
    if (SEGGER_RTT_Write(1U, wire, sizeof(wire)) != sizeof(wire)) { rx_rssi_dropped++; }
}

static void rx_rssi_stream_finish(uint32_t unique)
{
    rx_rssi_wire(rx_rssi_end, 2U, 0U, unique, rx_rssi_records, rx_rssi_dropped);
    rx_rssi_end_pending = 1U;
}

/* Retry the END marker while idle; it must not be silently dropped at shutdown. */
static void rx_rssi_stream_flush(void)
{
    if (rx_rssi_end_pending &&
        SEGGER_RTT_Write(1U, rx_rssi_end, sizeof(rx_rssi_end)) == sizeof(rx_rssi_end))
    {
        rx_rssi_end_pending = 0U;
    }
}
#endif
