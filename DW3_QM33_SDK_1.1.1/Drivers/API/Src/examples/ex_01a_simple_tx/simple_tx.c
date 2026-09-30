/*! ----------------------------------------------------------------------------
 *  @file    simple_tx.c
 *  @brief   Simple TX example code
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

#if defined(TEST_SIMPLE_TX)

extern void test_run_info(unsigned char *data);

/* Example application name */
#define APP_NAME "P2P 5NINES TX v1.0"

#define USE_SPI2 1 // set this to 1 to use DW37X0 SPI2

/* Five-nines packet-delivery experiment frame (FCS is appended by the DW IC):
 * byte 0       : 802.15.4e blink frame type.
 * byte 1       : low 8 bits of the sequence number.
 * bytes 2..5  : experiment magic "5NIN".
 * bytes 6..9  : 32-bit little-endian sequence number.
 * byte 10      : 0 for data, 1 for end-of-test marker.
 * bytes 11..31: deterministic payload checked by the receiver.
 */
#define TEST_FRAME_BYTES 32U
#define TEST_SEQUENCE_IDX 6U
#define TEST_TYPE_IDX 10U
#define TEST_PAYLOAD_IDX 11U
#define TEST_TYPE_DATA 0U
#define TEST_TYPE_END 1U
#define TEST_PACKET_TARGET 300000UL
#define TEST_END_MARKER_REPEATS 100U
static uint8_t tx_msg[TEST_FRAME_BYTES] = { 0 };

#define FRAME_LENGTH (sizeof(tx_msg) + FCS_LEN) // The real length that is going to be transmitted

/* Inter-frame delay period, in milliseconds. */
#define TX_DELAY_MS 10

/* Values for the PG_DELAY and TX_POWER registers reflect the bandwidth and power of the spectrum at the current
 * temperature. These values can be calibrated prior to taking reference measurements. See NOTE 2 below. */
extern dwt_config_t config_options;
extern dwt_txconfig_t txconfig_options;

static void prepare_test_frame(uint32_t sequence, uint8_t frame_type)
{
    uint32_t i;

    tx_msg[0] = 0xC5;
    tx_msg[1] = (uint8_t)sequence;
    tx_msg[2] = '5';
    tx_msg[3] = 'N';
    tx_msg[4] = 'I';
    tx_msg[5] = 'N';
    tx_msg[TEST_SEQUENCE_IDX + 0U] = (uint8_t)(sequence >> 0U);
    tx_msg[TEST_SEQUENCE_IDX + 1U] = (uint8_t)(sequence >> 8U);
    tx_msg[TEST_SEQUENCE_IDX + 2U] = (uint8_t)(sequence >> 16U);
    tx_msg[TEST_SEQUENCE_IDX + 3U] = (uint8_t)(sequence >> 24U);
    tx_msg[TEST_TYPE_IDX] = frame_type;

    for (i = TEST_PAYLOAD_IDX; i < TEST_FRAME_BYTES; i++)
    {
        tx_msg[i] = (uint8_t)(sequence + (i * 37U) + frame_type);
    }
}

/**
 * Application entry point.
 */
int simple_tx(void)
{
#if USE_SPI2
    uint8_t sema_res;
#endif
    uint32_t dev_id;
    uint32_t sequence = 0;
    uint32_t end_marker_count = 0;
    char report[128];

    /* Print application name on the console. */
    test_run_info((unsigned char *)APP_NAME);

    /* Configure SPI rate, DW3000 supports up to 38 MHz */
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

    dev_id = dwt_readdevid();
    if (dev_id == (uint32_t)DWT_DW3720_PDOA_DEV_ID)
    {
        /* If host is using SPI 2 to connect to DW3000 the code in the USE_SPI2 above should be set to 1 */
#if USE_SPI2
        change_SPI(SPI_2);

        /* Configure SPI rate, DW3000 supports up to 38 MHz */
        port_set_dw_ic_spi_fastrate();

        /* Reset DW IC */
        reset_DWIC(); /* Target specific drive of RSTn line into DW IC low for a period. */

        Sleep(2); // Time needed for DW3000 to start up (transition from INIT_RC to IDLE_RC, or could wait for SPIRDY event)

        /* If host is using SPI 2 to connect to DW3000 the it needs to request access or force access */

        sema_res = dwt_ds_sema_status();

        if ((sema_res & (0x2)) == 0) // the SPI2 is free
        {
            dwt_ds_sema_request();
        }
        else
        {
            test_run_info((unsigned char *)"SPI2 IS NOT FREE"); // If SPI2 is not free the host can force access
            while (1) { };
        }
#endif
    }

    while (!dwt_checkidlerc()) /* Need to make sure DW IC is in IDLE_RC before proceeding */ { };

    if (dwt_initialise(DWT_READ_OTP_ALL) == DWT_ERROR)
    {
        test_run_info((unsigned char *)"INIT FAILED     ");
        while (1) { };
    }

    /* If host is using SPI 2 to connect to DW3000 then the GPIOs are used for SPI2 and LEDs functionality cannot be used */
#if USE_SPI2 == 0
    /* Enabling LEDs here for debug so that for each TX the D1 LED will flash on DW3000 red eval-shield boards. */
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);
#endif

    /* Configure DW IC. See NOTE 5 below. */
    /* if the dwt_configure returns DWT_ERROR either the PLL or RX calibration has failed the host should reset the device */
    if (dwt_configure(&config_options))
    {
        test_run_info((unsigned char *)"CONFIG FAILED     ");
        while (1) { };
    }

    /* Configure the TX spectrum parameters (power PG delay and PG Count) */
    dwt_configuretxrf(&txconfig_options);

    snprintf(report, sizeof(report), "TEST START target=%lu interval_ms=%u channel=%u",
             (unsigned long)TEST_PACKET_TARGET, (unsigned int)TX_DELAY_MS, (unsigned int)config_options.chan);
    test_run_info((unsigned char *)report);

    /* Send the numbered test frames, then repeat an end marker so the receiver
     * can account for any packets lost at the end of the run. */
    while (sequence < TEST_PACKET_TARGET)
    {
        prepare_test_frame(sequence, TEST_TYPE_DATA);

        /* Write frame data to DW IC and prepare transmission. See NOTE 3 below.*/
        dwt_writetxdata(FRAME_LENGTH - FCS_LEN, tx_msg, 0); /* Zero offset in TX buffer. */

        /* In this example since the length of the transmitted frame does not change,
         * nor the other parameters of the dwt_writetxfctrl function, the
         * dwt_writetxfctrl call could be outside the main while(1) loop.
         */
        dwt_writetxfctrl(FRAME_LENGTH, 0, 0); /* Zero offset in TX buffer, no ranging. */

        /* Start transmission. */
        dwt_starttx(DWT_START_TX_IMMEDIATE);
        /* Poll DW IC until TX frame sent event set. See NOTE 4 below.
         * STATUS register is 4 bytes long but, as the event we are looking
         * at is in the first byte of the register, we can use this simplest
         * API function to access it.*/
        waitforsysstatus(NULL, NULL, DWT_INT_TXFRS_BIT_MASK, 0);

        /* Clear TX frame sent event. */
        dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);

        sequence++;

        if ((sequence % 1000UL) == 0UL)
        {
            snprintf(report, sizeof(report), "TX_PROGRESS sent=%lu target=%lu",
                     (unsigned long)sequence, (unsigned long)TEST_PACKET_TARGET);
            test_run_info((unsigned char *)report);
        }

        /* Execute a delay between transmissions. */
        Sleep(TX_DELAY_MS);
    }

    prepare_test_frame(TEST_PACKET_TARGET, TEST_TYPE_END);
    while (end_marker_count < TEST_END_MARKER_REPEATS)
    {
        dwt_writetxdata(FRAME_LENGTH - FCS_LEN, tx_msg, 0);
        dwt_writetxfctrl(FRAME_LENGTH, 0, 0);
        dwt_starttx(DWT_START_TX_IMMEDIATE);
        waitforsysstatus(NULL, NULL, DWT_INT_TXFRS_BIT_MASK, 0);
        dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);
        end_marker_count++;
        Sleep(TX_DELAY_MS);
    }

    snprintf(report, sizeof(report), "TX_COMPLETE sent=%lu end_markers=%u",
             (unsigned long)sequence, (unsigned int)end_marker_count);
    test_run_info((unsigned char *)report);

    while (1)
    {
        Sleep(1000);
    }
}

#endif
/*****************************************************************************************************************************************************
 * NOTES:
 *
 * 1. The device ID is a hard coded constant in the blink to keep the example simple but for a real product every device should have a unique ID.
 *    For development purposes it is possible to generate a DW IC unique ID by combining the Lot ID & Part Number values programmed into the
 *    DW IC during its manufacture. However there is no guarantee this will not conflict with someone else's implementation. We recommended that
 *    customers buy a block of addresses from the IEEE Registration Authority for their production items. See "EUI" in the DW IC User Manual.
 * 2. In a real application, for optimum performance within regulatory limits, it may be necessary to set TX pulse bandwidth and TX power, (using
 *    the dwt_configuretxrf API call) to per device calibrated values saved in the target system or the DW IC OTP memory.
 * 3. dwt_writetxdata() takes the full size of tx_msg as a parameter but only copies (size - 2) bytes as the check-sum at the end of the frame is
 *    automatically appended by the DW IC. This means that our tx_msg could be two bytes shorter without losing any data (but the sizeof would not
 *    work anymore then as we would still have to indicate the full length of the frame to dwt_writetxdata()).
 * 4. We use polled mode of operation here to keep the example as simple as possible, but the TXFRS status event can be used to generate an interrupt.
 *    Please refer to DW IC User Manual for more details on "interrupts".
 * 5. Desired configuration by user may be different to the current programmed configuration. dwt_configure is called to set desired
 *    configuration.
 ****************************************************************************************************************************************************/
