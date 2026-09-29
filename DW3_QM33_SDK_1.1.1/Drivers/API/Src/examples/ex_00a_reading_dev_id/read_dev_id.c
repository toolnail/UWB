
/*! ----------------------------------------------------------------------------
 * @file    read_dev_id.c
 * @brief   Read DW IC device ID and debug SPI communication.
 *
 * @author  Decawave
 *
 * @copyright SPDX-FileCopyrightText:
 *            Copyright (c) 2024 Qorvo US, Inc.
 *            SPDX-License-Identifier: LicenseRef-QORVO-2
 */

#include "deca_probe_interface.h"
#include <deca_device_api.h>
#include <example_selection.h>
#include <port.h>
#include <stdio.h>

#if defined(TEST_READING_DEV_ID)

extern void test_run_info(unsigned char *data);

/* Application name */
#define APP_NAME "READ DEV ID"

/* Set to 1 to enable SPI2 for DW37X0 */
#define USE_SPI2 1

/**
 * Application entry point.
 */
int read_dev_id(void)
{
#if USE_SPI2
    uint8_t sema_res;
#endif

    int err;
    uint32_t dev_id;
    char debug_msg[64];

    /* Step 1: Print application name */
    test_run_info((unsigned char *)APP_NAME);

    /* Step 2: Configure SPI */
    test_run_info((unsigned char *)"START SPI CONFIG");

    port_set_dw_ic_spi_fastrate();

    test_run_info((unsigned char *)"SPI RATE OK");

    /* Step 3: Reset DW IC */
    test_run_info((unsigned char *)"START RESET");

    reset_DWIC();

    test_run_info((unsigned char *)"RESET OK");

    /* Step 4: Wait for DW IC to start */
    Sleep(2);

    test_run_info((unsigned char *)"SLEEP OK");

    /* Step 5: Probe the device */
    test_run_info((unsigned char *)"START PROBE");

    if (dwt_probe(
            (struct dwt_probe_s *)&dw3000_probe_interf
        ) == DWT_ERROR)
    {
        test_run_info(
            (unsigned char *)"PROBE FAILED"
        );

        while (1)
        {
        }
    }

    test_run_info((unsigned char *)"PROBE OK");

    /* Step 6: Read Device ID */
    test_run_info((unsigned char *)"START READ DEV ID");

    dev_id = dwt_readdevid();

    snprintf(
        debug_msg,
        sizeof(debug_msg),
        "DEVICE ID = 0x%08lX",
        (unsigned long)dev_id
    );

    test_run_info((unsigned char *)debug_msg);

    /*
     * Step 7:
     * Switch to SPI2 if a DW3720 device is detected.
     */
    if (dev_id == (uint32_t)DWT_DW3720_PDOA_DEV_ID)
    {
#if USE_SPI2

        test_run_info(
            (unsigned char *)"START SWITCH SPI2"
        );

        change_SPI(SPI_2);

        port_set_dw_ic_spi_fastrate();

        reset_DWIC();

        Sleep(2);

        test_run_info(
            (unsigned char *)"SPI2 CONFIG OK"
        );

        /* Check SPI2 semaphore */
        sema_res = dwt_ds_sema_status();

        if ((sema_res & 0x2) == 0)
        {
            dwt_ds_sema_request();

            test_run_info(
                (unsigned char *)"SPI2 REQUEST OK"
            );
        }
        else
        {
            test_run_info(
                (unsigned char *)"SPI2 IS NOT FREE"
            );

            while (1)
            {
            }
        }

#endif
    }

    /* Step 8: Validate Device ID */
    test_run_info(
        (unsigned char *)"START CHECK DEV ID"
    );

    err = dwt_check_dev_id();

    if (err == DWT_SUCCESS)
    {
        test_run_info(
            (unsigned char *)"DEV ID OK"
        );
    }
    else
    {
        test_run_info(
            (unsigned char *)"DEV ID FAILED"
        );
    }

    test_run_info(
        (unsigned char *)"READ DEV ID FINISHED"
    );

    return err;
}

#endif
