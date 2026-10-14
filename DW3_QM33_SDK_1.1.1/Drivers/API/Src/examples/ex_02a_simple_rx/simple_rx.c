/*! ----------------------------------------------------------------------------
 *  @file    simple_rx.c
 *  @brief   Simple RX example code
 *
 * @author Decawave
 *
 * @copyright SPDX-FileCopyrightText: Copyright (c) 2024 Qorvo US, Inc.
 *            SPDX-License-Identifier: LicenseRef-QORVO-2
 *
 */

#include "deca_probe_interface.h"
#include <config_options.h>
#include <deca_device_api.h>
#include <deca_spi.h>
#include <example_selection.h>
#include <p2p_test_config.h>
#include <port.h>
#include <shared_defines.h>
#include <shared_functions.h>
#include <stdio.h>
#include <string.h>
#include <nrf.h>

#if defined(TEST_SIMPLE_RX)

extern void test_run_info(unsigned char *data);
#include "rx_signal_metrics.h"
#define RX_QUEUE_BARRIER() __DMB()
#include "rx_frame_queue.h"
#include "rx_rssi_stream.h"

/* Example application name */
#define APP_NAME "P2P 5NINES RX QUEUED SIGNAL v2.0"

/* Keep these byte offsets aligned with ex_01a_simple_tx/simple_tx.c. */
#define TEST_FRAME_BYTES 32U
#define TEST_SEQUENCE_IDX 1U
#define TEST_TYPE_IDX 5U
#define TEST_PAYLOAD_IDX 6U
#define TEST_APP_PAYLOAD_BYTES (TEST_FRAME_BYTES - TEST_PAYLOAD_IDX)
#define TEST_TIMING_IDX 24U
#define TEST_TIMING_BYTES 8U
#define TEST_TYPE_DATA 0U
#define TEST_TYPE_END 1U
#define TEST_FRAME_LENGTH (TEST_FRAME_BYTES + FCS_LEN)
#define TEST_RECEIVED_BITMAP_BYTES ((TEST_PACKET_TARGET + 7UL) / 8UL)

/* Buffer to store received frame. See NOTE 1 below. */
/* One bit per transmitted data sequence number, so late/out-of-order valid frames count once. */
static uint8_t received_bitmap[TEST_RECEIVED_BITMAP_BYTES];

static rx_frame_queue_t rx_queue;
static volatile uint32_t rx_reception_id;
static volatile uint32_t rx_epoch;
static volatile uint32_t phy_error_count;
static volatile uint32_t crc_error_count;
static volatile uint32_t rx_hw_overrun;
static volatile uint32_t rx_queue_dropped;
static volatile uint32_t rx_capture_dropped;
static volatile uint32_t rx_queue_high_water;
static volatile uint32_t rx_restart_errors;
static volatile uint8_t rx_recover_pending;
static uint32_t rx_stale_dropped;

static void rx_restart(void)
{
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS)
    {
        rx_restart_errors++;
        rx_recover_pending = 1U;
    }
}

/* ISR producer. The driver owns the hardware buffer until this callback returns.
 * Do NOT call dwt_signal_rx_buff_free here: dwt_isr does it after the callback.
 * Live CIA/DGC are copied before RX is re-enabled. The other hardware buffer
 * can then receive while we copy this frame to a privately reserved queue slot. */
static void rx_ok_cb(const dwt_cb_data_t *cb_data)
{
    uint32_t reception_id = ++rx_reception_id;
    rx_frame_record_t *record;
    uint32_t depth;

    if ((cb_data->status & DWT_INT_RXOVRR_BIT_MASK) || rx_recover_pending)
    {
        rx_capture_dropped++;
        rx_recover_pending = 1U;
        return;
    }
    if (cb_data->datalength != TEST_FRAME_LENGTH)
    {
        rx_capture_dropped++;
        rx_restart();
        return;
    }
    record = rx_frame_queue_reserve(&rx_queue);
    if (record == NULL)
    {
        rx_queue_dropped++;
        rx_restart();
        return;
    }
    record->reception_id = reception_id;
    record->epoch = rx_epoch;
    record->frame_len = cb_data->datalength;
    rx_signal_capture(&record->signal,
        (cb_data->rx_flags & DWT_CB_DATA_RX_FLAG_CIA) &&
        !(cb_data->rx_flags & DWT_CB_DATA_RX_FLAG_CER));
    rx_restart();
    dwt_readrxdata(record->frame, TEST_FRAME_BYTES, 0);
    /* An overrun invalidates ownership assumptions: never publish this record. */
    if (rx_recover_pending || (dwt_readsysstatuslo() & DWT_INT_RXOVRR_BIT_MASK))
    {
        rx_capture_dropped++;
        rx_recover_pending = 1U;
        return;
    }
    record->sequence = ((uint32_t)record->frame[TEST_SEQUENCE_IDX] << 0U)
                     | ((uint32_t)record->frame[TEST_SEQUENCE_IDX + 1U] << 8U)
                     | ((uint32_t)record->frame[TEST_SEQUENCE_IDX + 2U] << 16U)
                     | ((uint32_t)record->frame[TEST_SEQUENCE_IDX + 3U] << 24U);
    rx_frame_queue_commit(&rx_queue);
    depth = (uint32_t)(rx_queue.head - rx_queue.tail);
    if (depth > rx_queue_high_water) { rx_queue_high_water = depth; }
}

static void rx_err_cb(const dwt_cb_data_t *cb_data)
{
    phy_error_count++;
    if (cb_data->status & DWT_INT_RXFCE_BIT_MASK) { crc_error_count++; }
    if (cb_data->status & DWT_INT_RXOVRR_BIT_MASK)
    {
        rx_hw_overrun++;
        rx_recover_pending = 1U;
    }
    else if (!rx_recover_pending)
    {
        rx_restart();
    }
}

/* Reset buffer ownership only AFTER the SDK ISR has finished its own toggle.
 * The new epoch causes main to discard any queued records from the old epoch. */
static void rx_irq_handler(void)
{
    dwt_isr();
    if (rx_recover_pending)
    {
        dwt_forcetrxoff();
        dwt_setdblrxbuffmode(DBL_BUF_STATE_DIS, DBL_BUF_MODE_MAN);
        dwt_writerdbstatus(0xFFU);
        dwt_writesysstatuslo(SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO |
                            DWT_INT_RXOVRR_BIT_MASK | DWT_INT_RXFCG_BIT_MASK);
        dwt_setdblrxbuffmode(DBL_BUF_STATE_EN, DBL_BUF_MODE_MAN);
        rx_epoch++;
        rx_recover_pending = 0U;
        rx_restart();
    }
}

static void print_rx_pipeline(char *report, size_t size)
{
    snprintf(report, size,
        "RX_PIPELINE queue_size=%u queue_high_water=%lu queue_drop=%lu hw_overrun=%lu capture_drop=%lu stale_drop=%lu restart_err=%lu epoch=%lu",
        (unsigned int)TEST_RX_QUEUE_SIZE, (unsigned long)rx_queue_high_water,
        (unsigned long)rx_queue_dropped, (unsigned long)rx_hw_overrun,
        (unsigned long)rx_capture_dropped, (unsigned long)rx_stale_dropped,
        (unsigned long)rx_restart_errors, (unsigned long)rx_epoch);
    test_run_info((unsigned char *)report);
}

extern dwt_config_t config_options;

static int validate_test_frame(const uint8_t *frame, uint32_t sequence, uint8_t frame_type)
{
    uint32_t i;

    if ((frame[0] != 0xC5) || (frame_type > TEST_TYPE_END))
    {
        return 0;
    }

    for (i = TEST_PAYLOAD_IDX; i < TEST_FRAME_BYTES; i++)
    {
        /* The end marker carries the measured duration in its final eight bytes. */
        if ((frame_type == TEST_TYPE_END) && (i >= TEST_TIMING_IDX)) { break; }
        if (frame[i] != (uint8_t)(sequence + (i * 37U) + frame_type))
        {
            return 0;
        }
    }

    return 1;
}

static int mark_sequence_received(uint32_t sequence)
{
    uint32_t byte_index = sequence >> 3U;
    uint8_t mask = (uint8_t)(1U << (sequence & 7U));

    if ((received_bitmap[byte_index] & mask) != 0U)
    {
        return 0;
    }

    received_bitmap[byte_index] |= mask;
    return 1;
}

static void print_rx_report(char *report, size_t report_size, const char *prefix,
                            uint32_t valid, uint32_t lost, uint32_t duplicate,
                            uint32_t out_of_order, uint32_t phy_errors,
                            uint32_t crc_errors)
{
    uint32_t total = valid + lost;
    uint32_t loss_ppm = (total == 0U) ? 0U : (uint32_t)(((uint64_t)lost * 1000000ULL) / total);

    snprintf(report, report_size,
             "%s total=%lu valid=%lu lost=%lu dup=%lu ooo=%lu phy_err=%lu crc_err=%lu loss_ppm=%lu",
             prefix, (unsigned long)total, (unsigned long)valid, (unsigned long)lost,
             (unsigned long)duplicate, (unsigned long)out_of_order,
             (unsigned long)phy_errors, (unsigned long)crc_errors, (unsigned long)loss_ppm);
    test_run_info((unsigned char *)report);
}

static void print_rx_complete_report(char *report, size_t report_size,
                                     uint32_t valid, uint32_t duplicate,
                                     uint32_t out_of_order,
                                     uint32_t phy_errors, uint32_t crc_errors,
                                     uint32_t end_sequence, uint64_t tx_elapsed_us)
{
    rx_signal_report("RX_SIGNAL_COMPLETE", report, report_size);
    uint32_t failed = TEST_PACKET_TARGET - valid;
    /* Count all target sequences not successfully received, without classifying
     * their cause. PHY/CRC error events are separate diagnostics. */
    uint32_t lost = failed;
    uint32_t success_ppm = (uint32_t)(((uint64_t)valid * 1000000ULL) / TEST_PACKET_TARGET);
    uint32_t goodput_bps = (tx_elapsed_us == 0U) ? 0U :
        (uint32_t)((((uint64_t)valid * TEST_APP_PAYLOAD_BYTES * 8ULL * 1000000ULL) +
                    (tx_elapsed_us / 2U)) / tx_elapsed_us);

    snprintf(report, report_size,
             "RX_COMPLETE RESULT=%s target=%lu end_seq=%lu success=%lu failed=%lu lost=%lu success_ppm=%lu dup=%lu ooo=%lu phy_err=%lu crc_err=%lu tx_elapsed_us=%llu payload_bytes=%lu goodput_bps=%lu",
             (tx_elapsed_us == 0ULL) ? "TIME_ERROR" : ((failed == 0U) ? "PASS_95CI" : "FAIL"),
             (unsigned long)TEST_PACKET_TARGET, (unsigned long)end_sequence,
             (unsigned long)valid, (unsigned long)failed, (unsigned long)lost,
             (unsigned long)success_ppm, (unsigned long)duplicate, (unsigned long)out_of_order,
             (unsigned long)phy_errors,
             (unsigned long)crc_errors, (unsigned long long)tx_elapsed_us,
             (unsigned long)TEST_APP_PAYLOAD_BYTES, (unsigned long)goodput_bps);
    test_run_info((unsigned char *)report);
}

/**
 * Application entry point.
 */
int simple_rx(void)
{
    uint32_t device_id;
    dwt_callbacks_s callbacks = {0};
    uint32_t high_water = 0;
    uint32_t valid_count = 0;
    uint32_t duplicate_count = 0;
    uint32_t out_of_order_count = 0;
    uint32_t last_report_bucket = 0;
    int final_report_printed = 0;
    char report[384];

    /* Print application name on the console. */
    test_run_info((unsigned char *)APP_NAME);

    /* Configure SPI rate, DW IC supports up to 38 MHz */
    port_set_dw_ic_spi_fastrate();

    /* Reset DW IC */
    reset_DWIC(); /* Target specific drive of RSTn line into DW IC low for a period. */

    Sleep(2); // Time needed for DW3000 to start up (transition from INIT_RC to IDLE_RC, or could wait for SPIRDY event)

    /* Probe for the correct device driver. */
    if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) == DWT_ERROR)
    {
        test_run_info((unsigned char *)"PROBE FAILED");
        while (1) { };
    }

    while (!dwt_checkidlerc()) /* Need to make sure DW IC is in IDLE_RC before proceeding */ { };

    if (dwt_initialise(DWT_READ_OTP_ALL) == DWT_ERROR)
    {
        test_run_info((unsigned char *)"INIT FAILED");
        while (1) { };
    }

    /* Enabling LEDs here for debug so that for each RX-enable the D2 LED will flash on DW3000 red eval-shield boards. */
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

    /* Configure DW IC. */
    /* if the dwt_configure returns DWT_ERROR either the PLL or RX calibration has failed the host should reset the device */
    if (dwt_configure(&config_options))
    {
        test_run_info((unsigned char *)"CONFIG FAILED     ");
        while (1) { };
    }

    device_id = dwt_readdevid();
    if ((device_id != DWT_DW3000_DEV_ID) && (device_id != DWT_DW3000_PDOA_DEV_ID) &&
        (device_id != DWT_QM33110_DEV_ID) && (device_id != DWT_QM33120_PDOA_DEV_ID))
    {
        test_run_info((unsigned char *)"RX_ABORT reason=unsupported_device");
        while (1) { };
    }
    rx_frame_queue_init(&rx_queue);
    memset(received_bitmap, 0, sizeof(received_bitmap));
    rx_signal_init(device_id, config_options.rxCode);
    rx_rssi_stream_init(TEST_PACKET_TARGET);
    dwt_setdblrxbuffmode(DBL_BUF_STATE_EN, DBL_BUF_MODE_MAN);
    callbacks.cbRxOk = rx_ok_cb;
    callbacks.cbRxErr = rx_err_cb;
    callbacks.cbRxTo = rx_err_cb;
    dwt_setcallbacks(&callbacks);
    dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR |
                        SYS_STATUS_ALL_RX_TO | DWT_INT_RXOVRR_BIT_MASK |
                        DWT_INT_RCINIT_BIT_MASK | DWT_INT_SPIRDY_BIT_MASK);
    dwt_writerdbstatus(0xFFU);
    dwt_setinterrupt(DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR |
                     SYS_STATUS_ALL_RX_TO | DWT_INT_RXOVRR_BIT_MASK, 0, DWT_ENABLE_INT_ONLY);
    if ((device_id == DWT_QM33110_DEV_ID) || (device_id == DWT_QM33120_PDOA_DEV_ID))
    {
        dwt_setinterrupt_db(RDB_STATUS_RXOK, DWT_ENABLE_INT);
    }
    port_set_dwic_isr(rx_irq_handler);
    snprintf(report, sizeof(report),
             "RX_SIGNAL_CONFIG enabled=%u log_every=%lu history_size=%u source=IPATOV snapshot=PER_FRAME",
             (unsigned int)TEST_SIGNAL_ENABLE, (unsigned long)TEST_SIGNAL_LOG_EVERY,
             (unsigned int)TEST_SIGNAL_HISTORY_SIZE);
    test_run_info((unsigned char *)report);
    snprintf(report, sizeof(report),
             "RX_MODE device_id=0x%08lX double_buffer=1 reenable=MANUAL queue_size=%u",
             (unsigned long)device_id, (unsigned int)TEST_RX_QUEUE_SIZE);
    test_run_info((unsigned char *)report);
    rx_restart();
    snprintf(report, sizeof(report), "TEST START target=%lu channel=%u",
             (unsigned long)TEST_PACKET_TARGET, (unsigned int)config_options.chan);
    test_run_info((unsigned char *)report);

    /* Consumer: no SPI access here. An interrupt can append another independent
     * record while this slot is validated, calculated and printed. Keep the
     * current slot owned until all processing finishes, including END reports. */
    while (TRUE)
    {
        const rx_frame_record_t *record = rx_frame_queue_peek(&rx_queue);
        uint32_t sequence;
        uint8_t frame_type;
        const uint8_t *frame;
        rx_rssi_stream_flush();
        if (record == NULL) { continue; }
        if (record->epoch != rx_epoch)
        {
            rx_stale_dropped++;
            rx_frame_queue_release(&rx_queue);
            continue;
        }
        frame = record->frame;
        sequence = record->sequence;
        frame_type = frame[TEST_TYPE_IDX];
        if (!validate_test_frame(frame, sequence, frame_type))
        {
            rx_frame_queue_release(&rx_queue);
            continue;
        }
        if (frame_type == TEST_TYPE_END)
        {
            if ((sequence == TEST_PACKET_TARGET) && !final_report_printed)
            {
                uint64_t tx_elapsed_us = 0ULL;
                uint32_t i;
                for (i = 0U; i < TEST_TIMING_BYTES; i++)
                {
                    tx_elapsed_us |= (uint64_t)frame[TEST_TIMING_IDX + i] << (i * 8U);
                }
                print_rx_pipeline(report, sizeof(report));
                rx_rssi_stream_finish(valid_count);
                rx_rssi_stream_flush();
                print_rx_complete_report(report, sizeof(report), valid_count,
                                         duplicate_count, out_of_order_count,
                                         phy_error_count, crc_error_count,
                                         sequence, tx_elapsed_us);
                final_report_printed = 1;
            }
        }
        else if ((sequence < TEST_PACKET_TARGET) && !final_report_printed)
        {
            if (mark_sequence_received(sequence))
            {
                rx_signal_record_t signal;
                valid_count++;
                signal = rx_signal_process(sequence, record->reception_id, valid_count,
                                  &record->signal, report, sizeof(report));
                rx_rssi_stream_record(sequence, record->reception_id,
                                      signal.rssi_q8, signal.fp_power_q8, 0);
                if (sequence < high_water) { out_of_order_count++; }
                if (sequence >= high_water) { high_water = sequence + 1U; }
            }
            else
            {
                rx_signal_record_t signal = rx_signal_calculate(sequence,
                    record->reception_id, &record->signal);
                duplicate_count++;
                rx_rssi_stream_record(sequence, record->reception_id,
                                      signal.rssi_q8, signal.fp_power_q8, 1);
            }
        }
        if (!final_report_printed && ((valid_count / 1000UL) > last_report_bucket))
        {
            last_report_bucket = valid_count / 1000UL;
            print_rx_report(report, sizeof(report), "RX_PROGRESS",
                            valid_count, high_water - valid_count,
                            duplicate_count, out_of_order_count,
                            phy_error_count, crc_error_count);
            rx_signal_report("RX_SIGNAL_STATS", report, sizeof(report));
            print_rx_pipeline(report, sizeof(report));
        }
        rx_frame_queue_release(&rx_queue);
    }
}
#endif
/* Manual double-buffer reception: callbacks capture per-frame hardware inputs,
 * the SDK releases each RX buffer on callback return, and main owns queued
 * copies through validation/statistics/logging. No ACK/retry protocol is used. */
