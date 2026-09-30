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
#include <port.h>
#include <shared_defines.h>
#include <shared_functions.h>
#include <stdio.h>
#include <string.h>

#if defined(TEST_SIMPLE_RX)

extern void test_run_info(unsigned char *data);

/* Example application name */
#define APP_NAME "P2P 5NINES RX v1.0"

#define TEST_FRAME_BYTES 32U
#define TEST_SEQUENCE_IDX 6U
#define TEST_TYPE_IDX 10U
#define TEST_PAYLOAD_IDX 11U
#define TEST_TYPE_DATA 0U
#define TEST_TYPE_END 1U
#define TEST_PACKET_TARGET 300000UL
#define TEST_FRAME_LENGTH (TEST_FRAME_BYTES + FCS_LEN)
#define TEST_RECEIVED_BITMAP_BYTES ((TEST_PACKET_TARGET + 7UL) / 8UL)

/* Buffer to store received frame. See NOTE 1 below. */
static uint8_t rx_buffer[FRAME_LEN_MAX];
/* One bit per transmitted data sequence number, so late/out-of-order valid frames count once. */
static uint8_t received_bitmap[TEST_RECEIVED_BITMAP_BYTES];

extern dwt_config_t config_options;

static uint32_t read_sequence(const uint8_t *frame)
{
    return ((uint32_t)frame[TEST_SEQUENCE_IDX + 0U] << 0U)
           | ((uint32_t)frame[TEST_SEQUENCE_IDX + 1U] << 8U)
           | ((uint32_t)frame[TEST_SEQUENCE_IDX + 2U] << 16U)
           | ((uint32_t)frame[TEST_SEQUENCE_IDX + 3U] << 24U);
}

static int validate_test_frame(const uint8_t *frame, uint32_t sequence, uint8_t frame_type)
{
    uint32_t i;

    if ((frame[0] != 0xC5) || (frame[1] != (uint8_t)sequence)
        || (frame[2] != '5') || (frame[3] != 'N') || (frame[4] != 'I') || (frame[5] != 'N')
        || (frame_type > TEST_TYPE_END))
    {
        return 0;
    }

    for (i = TEST_PAYLOAD_IDX; i < TEST_FRAME_BYTES; i++)
    {
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
                            uint32_t out_of_order, uint32_t invalid, uint32_t phy_errors)
{
    uint32_t total = valid + lost;
    uint32_t loss_ppm = (total == 0U) ? 0U : (uint32_t)(((uint64_t)lost * 1000000ULL) / total);

    snprintf(report, report_size,
             "%s total=%lu valid=%lu lost=%lu dup=%lu ooo=%lu invalid=%lu phy_err=%lu loss_ppm=%lu",
             prefix, (unsigned long)total, (unsigned long)valid, (unsigned long)lost,
             (unsigned long)duplicate, (unsigned long)out_of_order, (unsigned long)invalid,
             (unsigned long)phy_errors, (unsigned long)loss_ppm);
    test_run_info((unsigned char *)report);
}

static void print_rx_complete_report(char *report, size_t report_size,
                                     uint32_t valid, uint32_t duplicate,
                                     uint32_t out_of_order, uint32_t invalid,
                                     uint32_t phy_errors)
{
    uint32_t failed = TEST_PACKET_TARGET - valid;
    uint32_t success_ppm = (uint32_t)(((uint64_t)valid * 1000000ULL) / TEST_PACKET_TARGET);

    snprintf(report, report_size,
             "RX_COMPLETE RESULT=%s target=%lu success=%lu failed=%lu success_ppm=%lu dup=%lu ooo=%lu invalid=%lu phy_err=%lu",
             (failed == 0U) ? "PASS_95CI" : "FAIL",
             (unsigned long)TEST_PACKET_TARGET, (unsigned long)valid, (unsigned long)failed,
             (unsigned long)success_ppm, (unsigned long)duplicate, (unsigned long)out_of_order,
             (unsigned long)invalid, (unsigned long)phy_errors);
    test_run_info((unsigned char *)report);
}

/**
 * Application entry point.
 */
int simple_rx(void)
{
    /* Hold copy of status register state here for reference so that it can be examined at a debug breakpoint. */
    uint32_t status_reg;
    /* Hold copy of frame length of frame received (if good) so that it can be examined at a debug breakpoint. */
    uint16_t frame_len;
    uint32_t expected_sequence = 0;
    uint32_t valid_count = 0;
    uint32_t lost_count = 0;
    uint32_t duplicate_count = 0;
    uint32_t out_of_order_count = 0;
    uint32_t invalid_count = 0;
    uint32_t phy_error_count = 0;
    uint32_t last_report_bucket = 0;
    int final_report_printed = 0;
    char report[192];

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

    snprintf(report, sizeof(report), "TEST START target=%lu channel=%u",
             (unsigned long)TEST_PACKET_TARGET, (unsigned int)config_options.chan);
    test_run_info((unsigned char *)report);

    /* Loop forever receiving frames. */
    while (TRUE)
    {
        /* TESTING BREAKPOINT LOCATION #1 */

        /* Clear local RX buffer to avoid having leftovers from previous receptions  This is not necessary but is included here to aid reading
         * the RX buffer.
         * This is a good place to put a breakpoint. Here (after first time through the loop) the local status register will be set for last event
         * and if a good receive has happened the data buffer will have the data in it, and frame_len will be set to the length of the RX frame. */
        memset(rx_buffer, 0, sizeof(rx_buffer));

        /* Activate reception immediately. See NOTE 2 below. */
        dwt_rxenable(DWT_START_RX_IMMEDIATE);

        /* Poll until a frame is properly received or an error/timeout occurs. See NOTE 3 below.
         * STATUS register is 5 bytes long but, as the event we are looking at is in the first byte of the register, we can use this simplest API
         * function to access it. */
        waitforsysstatus(&status_reg, NULL, (DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR), 0);

        if (status_reg & DWT_INT_RXFCG_BIT_MASK)
        {
            /* A frame has been received, copy it to our local buffer. */
            frame_len = dwt_getframelength(0);
            if ((frame_len == TEST_FRAME_LENGTH) && (frame_len <= FRAME_LEN_MAX))
            {
                dwt_readrxdata(rx_buffer, frame_len - FCS_LEN, 0); /* No need to read the FCS/CRC. */
            }

            /* Clear good RX frame event in the DW IC status register. */
            dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK);

            if (frame_len == TEST_FRAME_LENGTH)
            {
                uint32_t sequence = read_sequence(rx_buffer);
                uint8_t frame_type = rx_buffer[TEST_TYPE_IDX];

                if (!validate_test_frame(rx_buffer, sequence, frame_type))
                {
                    invalid_count++;
                }
                else if (frame_type == TEST_TYPE_END)
                {
                    if (!final_report_printed)
                    {
                        print_rx_complete_report(report, sizeof(report), valid_count,
                                                 duplicate_count, out_of_order_count,
                                                 invalid_count, phy_error_count);
                        final_report_printed = 1;
                    }
                }
                else if (sequence >= TEST_PACKET_TARGET)
                {
                    invalid_count++;
                }
                else
                {
                    if (mark_sequence_received(sequence))
                    {
                        valid_count++;
                    }

                    if (sequence == expected_sequence)
                    {
                        expected_sequence++;
                    }
                    else if (sequence > expected_sequence)
                    {
                        lost_count += sequence - expected_sequence;
                        expected_sequence = sequence + 1U;
                    }
                    else if (sequence + 1U == expected_sequence)
                    {
                        duplicate_count++;
                    }
                    else
                    {
                        out_of_order_count++;
                    }
                }

                if (!final_report_printed && ((valid_count / 1000UL) > last_report_bucket))
                {
                    last_report_bucket = valid_count / 1000UL;
                    print_rx_report(report, sizeof(report), "RX_PROGRESS",
                                    valid_count, lost_count, duplicate_count, out_of_order_count,
                                    invalid_count, phy_error_count);
                }
            }
            else
            {
                invalid_count++;
            }
        }
        else
        {
            /* Clear RX error events in the DW IC status register. */
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_ERR);
            phy_error_count++;
        }
    }
}
#endif
/*****************************************************************************************************************************************************
 * NOTES:
 *
 * 1. In this example, maximum frame length is set to 127 bytes which is 802.15.4 UWB standard maximum frame length. DW IC supports an extended
 *    frame length (up to 1023 bytes long) mode which is not used in this example.
 * 2. Manual reception activation is performed here but DW IC offers several features that can be used to handle more complex scenarios or to
 *    optimise system's overall performance (e.g. timeout after a given time, automatic re-enabling of reception in case of errors, etc.).
 * 3. We use polled mode of operation here to keep the example as simple as possible, but RXFCG and error/timeout status events can be used to generate
 *    interrupts. Please refer to DW IC User Manual for more details on "interrupts".
 ****************************************************************************************************************************************************/
