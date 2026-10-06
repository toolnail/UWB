/*! ----------------------------------------------------------------------------
 * @file reliable_p2p_rx.c
 * @brief Reliable UWB point-to-point receiver using 32-packet block ACKs.
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

#include "reliable_p2p_protocol.h"

#if defined(TEST_RELIABLE_P2P_RX)

extern void test_run_info(unsigned char *data);
extern dwt_txconfig_t txconfig_options;
extern dwt_config_t config_options;

#define APP_NAME "RELIABLE P2P RX v1.0"

static uint8_t rx_frame[FRAME_LEN_MAX];
static uint8_t tx_frame[RELIABLE_P2P_FRAME_BYTES];
/* Session-wide packet identity survives ACK-window changes and retransmissions. */
#define PACKET_BITMAP_BYTES ((RELIABLE_P2P_TARGET + 7UL) / 8UL)
static uint8_t received_bitmap[PACKET_BITMAP_BYTES];
static uint8_t invalid_bitmap[PACKET_BITMAP_BYTES];
static uint8_t mac_sequence = 0U;

static uint32_t current_session = 0U;
static uint32_t unique_count = 0U;
static uint32_t invalid_failed_count = 0U;
static uint32_t duplicate_count = 0U;
static uint32_t invalid_count = 0U;
static uint32_t phy_error_count = 0U;
static uint32_t next_progress = 10000U;
static int session_active = 0;
static int complete_reported = 0;

static int bitmap_contains(const uint8_t *bitmap, uint32_t sequence)
{
    return (bitmap[sequence >> 3U] & (uint8_t)(1U << (sequence & 7U))) != 0U;
}

static void bitmap_mark(uint8_t *bitmap, uint32_t sequence)
{
    bitmap[sequence >> 3U] |= (uint8_t)(1U << (sequence & 7U));
}

static uint32_t ack_bitmap(uint32_t window_base, uint32_t requested_mask)
{
    uint32_t bit;
    uint32_t bitmap = 0U;
    if ((window_base >= RELIABLE_P2P_TARGET) ||
        ((window_base % RELIABLE_P2P_WINDOW_SIZE) != 0U))
    {
        return 0U;
    }
    for (bit = 0U; bit < RELIABLE_P2P_WINDOW_SIZE; bit++)
    {
        uint32_t sequence = window_base + bit;
        if ((sequence < RELIABLE_P2P_TARGET) &&
            (requested_mask & (1UL << bit)) &&
            bitmap_contains(received_bitmap, sequence))
        {
            bitmap |= 1UL << bit;
        }
    }
    return bitmap;
}

static void reset_session(uint32_t session_id)
{
    current_session = session_id;
    memset(received_bitmap, 0, sizeof(received_bitmap));
    memset(invalid_bitmap, 0, sizeof(invalid_bitmap));
    unique_count = 0U;
    invalid_failed_count = 0U;
    duplicate_count = 0U;
    invalid_count = 0U;
    phy_error_count = 0U;
    next_progress = 10000U;
    session_active = 1;
    complete_reported = 0;
}

static void prepare_response(uint8_t type, uint32_t field,
                             uint32_t arg0, uint32_t arg1, uint32_t arg2,
                             uint32_t arg3, uint32_t arg4)
{
    reliable_p2p_prepare_frame(tx_frame, mac_sequence++,
                               (uint8_t)'X', (uint8_t)'T',
                               (uint8_t)'X', (uint8_t)'R',
                               type, current_session, field,
                               arg0, arg1, arg2, arg3, arg4);
}

static void send_response(void)
{
    dwt_writetxdata(RELIABLE_P2P_FRAME_BYTES - FCS_LEN, tx_frame, 0);
    dwt_writetxfctrl(RELIABLE_P2P_FRAME_BYTES, 0, 0);
    if (dwt_starttx(DWT_START_TX_IMMEDIATE) == DWT_SUCCESS)
    {
        waitforsysstatus(NULL, NULL, DWT_INT_TXFRS_BIT_MASK, 0);
        dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);
    }
}

static void handle_data_frame(uint32_t sequence)
{
    char report[176];

    if (sequence >= RELIABLE_P2P_TARGET)
    {
        invalid_count++;
        return;
    }
    if (!reliable_p2p_payload_is_valid(rx_frame, sequence))
    {
        invalid_count++;
        if (!bitmap_contains(invalid_bitmap, sequence) &&
            !bitmap_contains(received_bitmap, sequence))
        {
            invalid_failed_count++;
        }
        bitmap_mark(invalid_bitmap, sequence);
        return;
    }
    if (bitmap_contains(received_bitmap, sequence))
    {
        duplicate_count++;
        return;
    }

    bitmap_mark(received_bitmap, sequence);
    if (bitmap_contains(invalid_bitmap, sequence))
    {
        invalid_failed_count--;
    }
    unique_count++;

    if (unique_count >= next_progress)
    {
        snprintf(report, sizeof(report),
                 "RX_PROGRESS unique=%lu target=%lu duplicates=%lu invalid=%lu phy_err=%lu",
                 (unsigned long)unique_count,
                 (unsigned long)RELIABLE_P2P_TARGET,
                 (unsigned long)duplicate_count,
                 (unsigned long)invalid_count,
                 (unsigned long)phy_error_count);
        test_run_info((unsigned char *)report);
        next_progress += 10000U;
    }
}

int reliable_p2p_rx(void)
{
    uint32_t status_reg;
    uint16_t frame_len;
    char report[320];

    test_run_info((unsigned char *)APP_NAME);
    port_set_dw_ic_spi_fastrate();
    reset_DWIC();
    Sleep(2);

    if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) == DWT_ERROR)
    {
        test_run_info((unsigned char *)"PROBE FAILED");
        while (1) { };
    }
    while (!dwt_checkidlerc()) { };
    if (dwt_initialise(DWT_READ_OTP_ALL) == DWT_ERROR)
    {
        test_run_info((unsigned char *)"INIT FAILED");
        while (1) { };
    }
    if (dwt_configure(&config_options))
    {
        test_run_info((unsigned char *)"CONFIG FAILED");
        while (1) { };
    }
    dwt_configuretxrf(&txconfig_options);
    dwt_setpanid(0xDECAU);
    dwt_setaddress16(0x5258U);
    dwt_configureframefilter(DWT_FF_ENABLE_802_15_4, DWT_FF_DATA_EN);
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);
    dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR);

    snprintf(report, sizeof(report),
             "RX READY target=%lu window=%lu channel=%u",
             (unsigned long)RELIABLE_P2P_TARGET,
             (unsigned long)RELIABLE_P2P_WINDOW_SIZE,
             (unsigned int)config_options.chan);
    test_run_info((unsigned char *)report);

    while (1)
    {
        uint8_t type;
        uint32_t session;
        uint32_t field;

        status_reg = 0U;
        dwt_rxenable(DWT_START_RX_IMMEDIATE);
        waitforsysstatus(&status_reg, NULL,
                         DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR,
                         0);

        if ((status_reg & DWT_INT_RXFCG_BIT_MASK) == 0U)
        {
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_ERR);
            phy_error_count++;
            continue;
        }

        frame_len = dwt_getframelength(0);
        if (frame_len <= FRAME_LEN_MAX)
        {
            dwt_readrxdata(rx_frame, frame_len, 0);
        }
        dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK);

        if (!reliable_p2p_frame_is_valid(rx_frame, frame_len))
        {
            invalid_count++;
            continue;
        }

        type = rx_frame[RELIABLE_P2P_TYPE_IDX];
        session = reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_SESSION_IDX);
        field = reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_FIELD_IDX);

        if (type == RELIABLE_P2P_MSG_SESSION_START)
        {
            if ((field != RELIABLE_P2P_TARGET) ||
                (reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_ARG0_IDX) !=
                 RELIABLE_P2P_WINDOW_SIZE))
            {
                invalid_count++;
                continue;
            }
            if ((!session_active) || (session != current_session) || complete_reported)
            {
                reset_session(session);
            }
            prepare_response(RELIABLE_P2P_MSG_SESSION_ACK,
                             RELIABLE_P2P_TARGET,
                             RELIABLE_P2P_WINDOW_SIZE,
                             RELIABLE_P2P_TX_DELAY_MS,
                             0U, 0U, 0U);
            send_response();
            snprintf(report, sizeof(report),
                     "TEST START target=%lu interval_ms=%u window=%lu session=%08lX",
                     (unsigned long)RELIABLE_P2P_TARGET,
                     (unsigned int)RELIABLE_P2P_TX_DELAY_MS,
                     (unsigned long)RELIABLE_P2P_WINDOW_SIZE,
                     (unsigned long)current_session);
            test_run_info((unsigned char *)report);
            continue;
        }

        if ((!session_active) || (session != current_session))
        {
            invalid_count++;
            continue;
        }

        if (type == RELIABLE_P2P_MSG_DATA)
        {
            handle_data_frame(field);
        }
        else if (type == RELIABLE_P2P_MSG_BLOCK_ACK_REQ)
        {
            uint32_t requested_mask = reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_ARG0_IDX);
            uint32_t response_bitmap = ack_bitmap(field, requested_mask);
            prepare_response(RELIABLE_P2P_MSG_BLOCK_ACK, field,
                             response_bitmap, 0U, 0U, 0U, 0U);
            send_response();
        }
        else if (type == RELIABLE_P2P_MSG_SESSION_END)
        {
            if (field != RELIABLE_P2P_TARGET)
            {
                invalid_count++;
                continue;
            }
            uint32_t final_failed = RELIABLE_P2P_TARGET - unique_count;
            uint32_t invalid_failed = invalid_failed_count;
            /* Event counts may overlap packet failures or recovered retries. */
            uint32_t lost = final_failed - invalid_failed;
            uint32_t success_ppm = (uint32_t)(((uint64_t)unique_count * 1000000ULL) /
                                              RELIABLE_P2P_TARGET);

            prepare_response(RELIABLE_P2P_MSG_SUMMARY,
                             RELIABLE_P2P_TARGET,
                             unique_count,
                             final_failed,
                             duplicate_count,
                             invalid_count,
                             phy_error_count);
            send_response();

            if (!complete_reported)
            {
                snprintf(report, sizeof(report),
                         "RX_COMPLETE RESULT=%s target=%lu unique=%lu failed=%lu lost=%lu invalid_failed=%lu success_ppm=%lu duplicates=%lu invalid=%lu phy_err=%lu",
                         (final_failed == 0U) ? "PASS" : "FAIL",
                         (unsigned long)RELIABLE_P2P_TARGET,
                         (unsigned long)unique_count,
                         (unsigned long)final_failed,
                         (unsigned long)lost,
                         (unsigned long)invalid_failed,
                         (unsigned long)success_ppm,
                         (unsigned long)duplicate_count,
                         (unsigned long)invalid_count,
                         (unsigned long)phy_error_count);
                test_run_info((unsigned char *)report);
                complete_reported = 1;
            }
        }
        else
        {
            invalid_count++;
        }
    }
}

#endif /* TEST_RELIABLE_P2P_RX */
