/*! ----------------------------------------------------------------------------
 * @file reliable_p2p_tx.c
 * @brief Reliable UWB point-to-point transmitter using 32-packet block ACKs.
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

#include "reliable_p2p_protocol.h"

#if defined(TEST_RELIABLE_P2P_TX)

extern void test_run_info(unsigned char *data);
extern dwt_txconfig_t txconfig_options;
extern dwt_config_t config_options;

#define APP_NAME                    "RELIABLE P2P TX v1.0"
#define TX_TO_RX_DELAY_UUS          0U
#define RESPONSE_TIMEOUT_UUS        5000U

static uint8_t tx_frame[RELIABLE_P2P_FRAME_BYTES];
static uint8_t rx_frame[FRAME_LEN_MAX];
static uint8_t mac_sequence = 0U;

static uint32_t data_attempts = 0U;
static uint32_t retransmissions = 0U;
static uint32_t ack_requests = 0U;
static uint32_t ack_timeouts = 0U;
static uint32_t invalid_responses = 0U;
static uint32_t retried_windows = 0U;

static void prepare_tx_frame(uint8_t type, uint32_t field,
                             uint32_t arg0, uint32_t arg1, uint32_t arg2,
                             uint32_t arg3, uint32_t arg4)
{
    reliable_p2p_prepare_frame(tx_frame, mac_sequence++,
                               (uint8_t)'X', (uint8_t)'R',
                               (uint8_t)'X', (uint8_t)'T',
                               type, RELIABLE_P2P_SESSION_ID, field,
                               arg0, arg1, arg2, arg3, arg4);
}

static int transmit_only(void)
{
    dwt_writetxdata(RELIABLE_P2P_FRAME_BYTES - FCS_LEN, tx_frame, 0);
    dwt_writetxfctrl(RELIABLE_P2P_FRAME_BYTES, 0, 0);
    if (dwt_starttx(DWT_START_TX_IMMEDIATE) == DWT_ERROR)
    {
        return 0;
    }
    waitforsysstatus(NULL, NULL, DWT_INT_TXFRS_BIT_MASK, 0);
    dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);
    return 1;
}

static int transmit_and_get_response(uint8_t request_type,
                                     uint32_t field,
                                     uint32_t arg0,
                                     uint8_t expected_type,
                                     uint32_t *response_arg0,
                                     uint32_t *response_arg1,
                                     uint32_t *response_arg2,
                                     uint32_t *response_arg3,
                                     uint32_t *response_arg4)
{
    uint32_t status_reg = 0U;
    uint16_t frame_len;

    prepare_tx_frame(request_type, field, arg0, 0U, 0U, 0U, 0U);
    dwt_writetxdata(RELIABLE_P2P_FRAME_BYTES - FCS_LEN, tx_frame, 0);
    dwt_writetxfctrl(RELIABLE_P2P_FRAME_BYTES, 0, 0);

    if (dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED) == DWT_ERROR)
    {
        invalid_responses++;
        return 0;
    }

    waitforsysstatus(&status_reg, NULL,
                     DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR,
                     0);

    if ((status_reg & DWT_INT_RXFCG_BIT_MASK) == 0U)
    {
        dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        ack_timeouts++;
        return 0;
    }

    frame_len = dwt_getframelength(0);
    if (frame_len <= FRAME_LEN_MAX)
    {
        dwt_readrxdata(rx_frame, frame_len, 0);
    }
    dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK | DWT_INT_RXFCG_BIT_MASK);

    if (!reliable_p2p_frame_is_valid(rx_frame, frame_len) ||
        (rx_frame[RELIABLE_P2P_TYPE_IDX] != expected_type) ||
        (reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_SESSION_IDX) != RELIABLE_P2P_SESSION_ID) ||
        (reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_FIELD_IDX) != field))
    {
        invalid_responses++;
        return 0;
    }

    if (response_arg0 != NULL) { *response_arg0 = reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_ARG0_IDX); }
    if (response_arg1 != NULL) { *response_arg1 = reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_ARG1_IDX); }
    if (response_arg2 != NULL) { *response_arg2 = reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_ARG2_IDX); }
    if (response_arg3 != NULL) { *response_arg3 = reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_ARG3_IDX); }
    if (response_arg4 != NULL) { *response_arg4 = reliable_p2p_get_u32(rx_frame, RELIABLE_P2P_ARG4_IDX); }
    return 1;
}

static int start_session(void)
{
    uint32_t attempt;
    uint32_t response_window = 0U;

    for (attempt = 0U; attempt < RELIABLE_P2P_CONTROL_RETRIES; attempt++)
    {
        if (transmit_and_get_response(RELIABLE_P2P_MSG_SESSION_START,
                                      RELIABLE_P2P_TARGET,
                                      RELIABLE_P2P_WINDOW_SIZE,
                                      RELIABLE_P2P_MSG_SESSION_ACK,
                                      &response_window, NULL, NULL, NULL, NULL) &&
            (response_window == RELIABLE_P2P_WINDOW_SIZE))
        {
            return 1;
        }
        Sleep(2);
    }
    return 0;
}

int reliable_p2p_tx(void)
{
    uint32_t window_base;
    uint32_t final_failed = 0U;
    uint32_t next_progress = 10000U;
    uint32_t remote_unique = 0U;
    uint32_t remote_failed = RELIABLE_P2P_TARGET;
    uint32_t remote_duplicates = 0U;
    uint32_t remote_invalid = 0U;
    uint32_t remote_phy_errors = 0U;
    uint32_t control_attempt;
    int summary_received = 0;
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
    dwt_setrxaftertxdelay(TX_TO_RX_DELAY_UUS);
    dwt_setrxtimeout(RESPONSE_TIMEOUT_UUS);
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    snprintf(report, sizeof(report),
             "TEST START target=%lu interval_ms=%u window=%lu max_rounds=%u session=%08lX",
             (unsigned long)RELIABLE_P2P_TARGET,
             (unsigned int)RELIABLE_P2P_TX_DELAY_MS,
             (unsigned long)RELIABLE_P2P_WINDOW_SIZE,
             (unsigned int)RELIABLE_P2P_MAX_WINDOW_ROUNDS,
             (unsigned long)RELIABLE_P2P_SESSION_ID);
    test_run_info((unsigned char *)report);

    if (!start_session())
    {
        test_run_info((unsigned char *)"TX_ABORT reason=session_handshake_failed");
        while (1) { Sleep(1000); };
    }

    for (window_base = 0U; window_base < RELIABLE_P2P_TARGET;
         window_base += RELIABLE_P2P_WINDOW_SIZE)
    {
        uint32_t valid_mask = reliable_p2p_valid_mask(window_base);
        uint32_t acknowledged_mask = 0U;
        uint32_t round;

        for (round = 0U;
             (round < RELIABLE_P2P_MAX_WINDOW_ROUNDS) &&
             ((acknowledged_mask & valid_mask) != valid_mask);
             round++)
        {
            uint32_t bit;
            uint32_t response_bitmap = 0U;

            if (round == 1U)
            {
                retried_windows++;
            }

            for (bit = 0U; bit < RELIABLE_P2P_WINDOW_SIZE; bit++)
            {
                uint32_t bit_mask = 1UL << bit;
                uint32_t sequence = window_base + bit;

                if (((valid_mask & bit_mask) == 0U) ||
                    ((acknowledged_mask & bit_mask) != 0U))
                {
                    continue;
                }

                prepare_tx_frame(RELIABLE_P2P_MSG_DATA, sequence,
                                 0U, 0U, 0U, 0U, 0U);
                reliable_p2p_fill_payload(tx_frame, sequence);
                if (transmit_only())
                {
                    data_attempts++;
                    if (round > 0U)
                    {
                        retransmissions++;
                    }
                }
                Sleep(RELIABLE_P2P_TX_DELAY_MS);
            }

            ack_requests++;
            if (transmit_and_get_response(RELIABLE_P2P_MSG_BLOCK_ACK_REQ,
                                          window_base,
                                          valid_mask,
                                          RELIABLE_P2P_MSG_BLOCK_ACK,
                                          &response_bitmap, NULL, NULL, NULL, NULL))
            {
                acknowledged_mask |= response_bitmap & valid_mask;
            }
        }

        final_failed += reliable_p2p_popcount(valid_mask & ~acknowledged_mask);

        if ((window_base + RELIABLE_P2P_WINDOW_SIZE) >= next_progress)
        {
            uint32_t completed = window_base + RELIABLE_P2P_WINDOW_SIZE;
            if (completed > RELIABLE_P2P_TARGET) { completed = RELIABLE_P2P_TARGET; }
            snprintf(report, sizeof(report),
                     "TX_PROGRESS completed=%lu target=%lu final_failed=%lu attempts=%lu retransmissions=%lu ack_timeouts=%lu",
                     (unsigned long)completed,
                     (unsigned long)RELIABLE_P2P_TARGET,
                     (unsigned long)final_failed,
                     (unsigned long)data_attempts,
                     (unsigned long)retransmissions,
                     (unsigned long)ack_timeouts);
            test_run_info((unsigned char *)report);
            next_progress += 10000U;
        }
    }

    for (control_attempt = 0U;
         control_attempt < RELIABLE_P2P_CONTROL_RETRIES;
         control_attempt++)
    {
        if (transmit_and_get_response(RELIABLE_P2P_MSG_SESSION_END,
                                      RELIABLE_P2P_TARGET,
                                      final_failed,
                                      RELIABLE_P2P_MSG_SUMMARY,
                                      &remote_unique,
                                      &remote_failed,
                                      &remote_duplicates,
                                      &remote_invalid,
                                      &remote_phy_errors))
        {
            summary_received = 1;
            break;
        }
        Sleep(2);
    }

    snprintf(report, sizeof(report),
             "TX_REMOTE summary_received=%u unique=%lu failed=%lu duplicates=%lu invalid=%lu phy_err=%lu",
             (unsigned int)summary_received,
             (unsigned long)remote_unique,
             (unsigned long)remote_failed,
             (unsigned long)remote_duplicates,
             (unsigned long)remote_invalid,
             (unsigned long)remote_phy_errors);
    test_run_info((unsigned char *)report);

    snprintf(report, sizeof(report),
             "TX_COMPLETE RESULT=%s target=%lu success=%lu failed=%lu ack_missing=%lu attempts=%lu retransmissions=%lu ack_requests=%lu ack_timeouts=%lu invalid_responses=%lu retried_windows=%lu",
             (summary_received && (remote_unique + remote_failed == RELIABLE_P2P_TARGET) &&
              (remote_failed == 0U)) ? "PASS" : "FAIL",
             (unsigned long)RELIABLE_P2P_TARGET,
             (unsigned long)remote_unique,
             (unsigned long)remote_failed,
             (unsigned long)final_failed,
             (unsigned long)data_attempts,
             (unsigned long)retransmissions,
             (unsigned long)ack_requests,
             (unsigned long)ack_timeouts,
             (unsigned long)invalid_responses,
             (unsigned long)retried_windows);
    test_run_info((unsigned char *)report);

    while (1) { Sleep(1000); };
}

#endif /* TEST_RELIABLE_P2P_TX */
