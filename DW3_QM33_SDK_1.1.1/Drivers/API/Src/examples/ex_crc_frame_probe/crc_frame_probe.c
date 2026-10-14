/* Independent NATURAL CRC-error RX-buffer experiment.
 * TX uses the original simple_tx DATA format, normal hardware FCS and 500 us
 * delay; no error injection, alternating frame kinds, or packet target.
 * Original production sources/project remain unchanged. */
#include "deca_probe_interface.h"
#include <config_options.h>
#include <deca_device_api.h>
#include <deca_spi.h>
#include <examples_defines.h>
#include <port.h>
#include <shared_defines.h>
#include <shared_functions.h>
#include "dw3000_deca_regs.h"
#include <stdio.h>
#include <string.h>

#ifndef CRC_PROBE_ROLE_TX
#error Build this independent file using run_crc_frame_probe.ps1
#endif
#ifndef CRC_PROBE_RX_CHECK
#define CRC_PROBE_RX_CHECK 1
#endif
#define PROBE_BODY_BYTES 32U
#define PROBE_FRAME_BYTES (PROBE_BODY_BYTES + FCS_LEN)
#define PROBE_GAP_US 500U

extern void test_run_info(unsigned char *data);
extern dwt_config_t config_options;
extern dwt_txconfig_t txconfig_options;
example_ptr example_pointer;
static volatile uint32_t probe_spi_read_errors;
static void probe_spi_error(void) { probe_spi_read_errors++; }

/* Byte-for-byte identical to prepare_test_frame(seq, TEST_TYPE_DATA) in the
 * original simple_tx.c. FCS is appended by the chip, never supplied/altered. */
static void probe_make_frame(uint8_t *frame, uint32_t seq)
{
    uint32_t i;
    frame[0] = 0xC5U;
    for (i = 0U; i < 4U; i++) { frame[1U + i] = (uint8_t)(seq >> (i * 8U)); }
    frame[5] = 0U;
    for (i = 6U; i < PROBE_BODY_BYTES; i++) { frame[i] = (uint8_t)(seq + i * 37U); }
}

static int probe_init(void)
{
    uint32_t cfg;
    char report[144];
    port_set_dw_ic_spi_fastrate();
    reset_DWIC();
    Sleep(2);
    if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) == DWT_ERROR) { return 0; }
    while (!dwt_checkidlerc()) { }
    if (dwt_initialise(DWT_READ_OTP_ALL) != DWT_SUCCESS) { return 0; }
    if (dwt_configure(&config_options) != DWT_SUCCESS) { return 0; }
#if CRC_PROBE_ROLE_TX || CRC_PROBE_RX_CHECK
    dwt_configtxrxfcs(DWT_FCS_ENABLE);
#else
    /* API Guide 4.12, section 5.2.45: disable RX checking only.
     * The ordinary transmitter still generates the on-air FCS. */
    dwt_configtxrxfcs(DWT_FCS_RX_OFF);
#endif
    dwt_setdblrxbuffmode(DBL_BUF_STATE_DIS, DBL_BUF_MODE_MAN);
    dwt_setrxtimeout(0U);
#if !CRC_PROBE_ROLE_TX
    port_set_dw_ic_spi_slowrate();
    /* Verify host/chip SPI transfers independently of the radio frame FCS. */
    dwt_enablespicrccheck(DWT_SPI_CRC_MODE_WRRD, probe_spi_error);
#endif
    cfg = dwt_read_reg(SYS_CFG_ID);
    snprintf(report, sizeof(report), "PROBE_INIT sys_cfg=0x%08lX spi_read_errors=%lu role_tx=%u",
             (unsigned long)cfg, (unsigned long)probe_spi_read_errors, (unsigned int)CRC_PROBE_ROLE_TX);
    test_run_info((unsigned char *)report);
    if ((cfg & (SYS_CFG_DIS_FCS_TX_BIT_MASK |
         SYS_CFG_DIS_FCE_BIT_MASK | SYS_CFG_DIS_DRXB_BIT_MASK |
         SYS_CFG_RXAUTR_BIT_MASK | SYS_CFG_SPI_CRC_BIT_MASK)) !=
        (SYS_CFG_DIS_DRXB_BIT_MASK | (CRC_PROBE_ROLE_TX ? 0U : SYS_CFG_SPI_CRC_BIT_MASK) |
         ((!CRC_PROBE_ROLE_TX && !CRC_PROBE_RX_CHECK) ? SYS_CFG_DIS_FCE_BIT_MASK : 0U)) || probe_spi_read_errors)
    { return 0; }
    return 1;
}

#if CRC_PROBE_ROLE_TX
volatile uint32_t crc_probe_run = 1U;
static int probe_tx(void)
{
    uint8_t frame[PROBE_BODY_BYTES];
    uint32_t seq = 0U;
    char report[128];
    if (!probe_init()) { test_run_info((unsigned char *)"NATURAL_CRC_TX INIT_FAILED"); while (1) { } }
    dwt_configuretxrf(&txconfig_options);
    test_run_info((unsigned char *)"NATURAL_CRC_TX READY hardware_fcs=ON interval_us=500 original_data_format=1 no_packet_target=1");
    while (crc_probe_run)
    {
        uint32_t status;
        probe_make_frame(frame, seq);
        dwt_writetxdata(PROBE_BODY_BYTES, frame, 0U);
        dwt_writetxfctrl(PROBE_FRAME_BYTES, 0U, 0U);
        if (dwt_starttx(DWT_START_TX_IMMEDIATE) != DWT_SUCCESS)
        {
            test_run_info((unsigned char *)"NATURAL_CRC_TX START_FAILED");
            break;
        }
        waitforsysstatus(&status, NULL, DWT_INT_TXFRS_BIT_MASK, 0U);
        dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);
        seq++;
        if ((seq % 1000U) == 0U)
        {
            snprintf(report, sizeof(report), "TX_PROBE_PROGRESS sent=%lu", (unsigned long)seq);
            test_run_info((unsigned char *)report);
        }
        deca_usleep(PROBE_GAP_US);
    }
    test_run_info((unsigned char *)"NATURAL_CRC_TX STOPPED");
    while (1) { Sleep(1000); }
}
#else
static uint16_t probe_fcs(const uint8_t *data)
{
    uint16_t crc = 0U;
    unsigned int i, bit;
    for (i = 0U; i < PROBE_BODY_BYTES; i++)
    {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; bit++)
        { crc = (crc >> 1U) ^ ((crc & 1U) ? 0x8408U : 0U); }
    }
    return crc;
}
static uint32_t probe_sequence(const uint8_t *frame)
{
    return (uint32_t)frame[1] | ((uint32_t)frame[2] << 8U) |
           ((uint32_t)frame[3] << 16U) | ((uint32_t)frame[4] << 24U);
}
static void probe_dump(const char *label, const uint8_t *data)
{
    char line[120];
    unsigned int i;
    int offset = snprintf(line, sizeof(line), "%s hex=", label);
    for (i = 0U; i < PROBE_FRAME_BYTES; i++)
    {
        offset += snprintf(line + offset, sizeof(line) - (size_t)offset, "%02X", (unsigned int)data[i]);
    }
    test_run_info((unsigned char *)line);
}
static unsigned int probe_payload_matches(const uint8_t *frame, uint32_t seq)
{
    unsigned int i, count = 0U;
    for (i = 6U; i < PROBE_BODY_BYTES; i++)
    {
        if (frame[i] == (uint8_t)(seq + i * 37U)) { count++; }
    }
    return count;
}
static int probe_rx(void)
{
    uint8_t before[PROBE_FRAME_BYTES], after[PROBE_FRAME_BYTES], repeat[PROBE_FRAME_BYTES];
    uint8_t before_arm[PROBE_FRAME_BYTES];
    uint8_t previous[PROBE_FRAME_BYTES] = {0}, expected[PROBE_BODY_BYTES];
    uint32_t previous_seq = 0U, good = 0U, errors = 0U, other = 0U;
    uint32_t pending_event = 0U;
    int have_previous = 0;
    char report[512];
    if (!probe_init()) { test_run_info((unsigned char *)"NATURAL_CRC_RX INIT_FAILED"); while (1) { } }
    snprintf(report, sizeof(report), "NATURAL_CRC_RX READY device=0x%08lX single_buffer=1 crc_check=%s frame_bytes=%u sys_cfg=0x%08lX spi_crc=WRRD spi_read_errors=%lu",
             (unsigned long)dwt_readdevid(), CRC_PROBE_RX_CHECK ? "ON" : "OFF", (unsigned int)PROBE_FRAME_BYTES,
             (unsigned long)dwt_read_reg(SYS_CFG_ID), (unsigned long)probe_spi_read_errors);
    test_run_info((unsigned char *)report);
    while (1)
    {
        uint32_t status;
        uint16_t frame_length;
        dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | DWT_INT_RXFR_BIT_MASK | SYS_STATUS_ALL_RX_ERR);
        /* Direct on-chip baseline immediately before this one RX attempt,
         * even if an earlier attempt ended in another PHY/CRC error. */
        memset(before_arm, 0x69, sizeof(before_arm));
        dwt_readrxdata(before_arm, sizeof(before_arm), 0U);
        dwt_rxenable(DWT_START_RX_IMMEDIATE);
        waitforsysstatus(&status, NULL, DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR |
                        (CRC_PROBE_RX_CHECK ? 0U : DWT_INT_RXFR_BIT_MASK), 0U);
        if (status & (DWT_INT_RXFCG_BIT_MASK | DWT_INT_RXFCE_BIT_MASK |
                      (CRC_PROBE_RX_CHECK ? 0U : DWT_INT_RXFR_BIT_MASK)))
        {
            uint16_t calculated_fcs, read_fcs;
            frame_length = dwt_getframelength(0U);
            /* A fresh SPI read, independent of whether RX_FINFO is up to date.
             * 34 is the known TX frame length. Do not rely on an old MCU array. */
            memset(before, 0xA5, sizeof(before));
            dwt_readrxdata(before, sizeof(before), 0U);
            calculated_fcs = probe_fcs(before);
            read_fcs = (uint16_t)before[32] | ((uint16_t)before[33] << 8U);
            if (((status & DWT_INT_RXFCE_BIT_MASK) && !(status & DWT_INT_RXFCG_BIT_MASK)) ||
                (!CRC_PROBE_RX_CHECK && (status & DWT_INT_RXFR_BIT_MASK) &&
                 frame_length == PROBE_FRAME_BYTES && calculated_fcs != read_fcs))
            {
                uint32_t cfg, finfo, state, status_before_clear, status_after_clear;
                uint32_t observed_seq = probe_sequence(before);
                unsigned int payload_matches = probe_payload_matches(before, observed_seq);
                int previous_before = have_previous && memcmp(before, previous, sizeof(before)) == 0;
                int previous_after;
                /* Receiver is still stopped: no rearm/reset between snapshots. */
                memset(repeat, 0x3C, sizeof(repeat));
                dwt_readrxdata(repeat, sizeof(repeat), 0U);
                cfg = dwt_read_reg(SYS_CFG_ID);
                finfo = dwt_read_reg(RX_FINFO_ID);
                state = dwt_read_reg(SYS_STATE_LO_ID);
                status_before_clear = dwt_readsysstatuslo();
                dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | DWT_INT_RXFR_BIT_MASK | SYS_STATUS_ALL_RX_ERR);
                memset(after, 0x5A, sizeof(after));
                dwt_readrxdata(after, sizeof(after), 0U);
                status_after_clear = dwt_readsysstatuslo();
                previous_after = have_previous && memcmp(after, previous, sizeof(after)) == 0;
                errors++;
                pending_event = errors;
                snprintf(report, sizeof(report),
                         "RX_PROBE_CRC event=%lu status=0x%08lX frame_len=%u have_previous=%u prev_seq=%lu observed_seq=%lu payload_matches=%u pre_previous=%u post_previous=%u same_reads=%u good=%lu other=%lu source=NATURAL mode=%s cause=%s calculated_fcs=0x%04X read_fcs=0x%04X",
                         (unsigned long)errors, (unsigned long)status, (unsigned int)frame_length,
                         (unsigned int)have_previous, (unsigned long)previous_seq, (unsigned long)observed_seq,
                         payload_matches, (unsigned int)previous_before, (unsigned int)previous_after,
                         (unsigned int)(memcmp(before, after, sizeof(before)) == 0),
                         (unsigned long)good, (unsigned long)other,
                         CRC_PROBE_RX_CHECK ? "ON" : "OFF",
                         CRC_PROBE_RX_CHECK ? "HARDWARE_RXFCE" : "SOFTWARE_CRC_MISMATCH",
                         (unsigned int)calculated_fcs, (unsigned int)read_fcs);
                test_run_info((unsigned char *)report);
                probe_dump("CRC_BEFORE_CLEAR", before);
                probe_dump("CRC_AFTER_CLEAR", after);
                probe_dump("CRC_PREVIOUS_GOOD", previous);
                snprintf(report, sizeof(report),
                         "CRC_CHECKS event=%lu sys_cfg=0x%08lX rx_finfo=0x%08lX sys_state=0x%08lX status_pre=0x%08lX status_post=0x%08lX repeat_equal=%u spi_read_errors=%lu spi_write_error=%u",
                         (unsigned long)errors, (unsigned long)cfg, (unsigned long)finfo,
                         (unsigned long)state, (unsigned long)status_before_clear,
                         (unsigned long)status_after_clear,
                         (unsigned int)(memcmp(before, repeat, sizeof(before)) == 0),
                         (unsigned long)probe_spi_read_errors,
                         (unsigned int)(((status | status_before_clear | status_after_clear) & SYS_STATUS_SPICRCE_BIT_MASK) != 0U));
                test_run_info((unsigned char *)report);
                probe_dump("CRC_BEFORE_ARM", before_arm);
            }
            else if (status & (DWT_INT_RXFCG_BIT_MASK | (CRC_PROBE_RX_CHECK ? 0U : DWT_INT_RXFR_BIT_MASK)))
            {
                uint32_t seq = probe_sequence(before);
                int verified;
                probe_make_frame(expected, seq);
                verified = frame_length == PROBE_FRAME_BYTES && memcmp(before, expected, sizeof(expected)) == 0 &&
                           calculated_fcs == read_fcs && probe_spi_read_errors == 0U && !(status & SYS_STATUS_SPICRCE_BIT_MASK);
                if (verified)
                {
                    good++;
                    if (pending_event || good == 1U)
                    {
                        snprintf(report, sizeof(report), "RX_PROBE_GOOD seq=%lu status=0x%08lX frame_len=%u verified=1 after_crc_event=%lu",
                                 (unsigned long)seq, (unsigned long)status, (unsigned int)frame_length, (unsigned long)pending_event);
                        test_run_info((unsigned char *)report);
                    }
                    memcpy(previous, before, sizeof(previous));
                    previous_seq = seq;
                    have_previous = 1;
                    pending_event = 0U;
                    if ((good % 1000U) == 0U)
                    {
                        snprintf(report, sizeof(report), "RX_PROBE_PROGRESS good=%lu last_seq=%lu crc_errors=%lu other_errors=%lu",
                                 (unsigned long)good, (unsigned long)seq, (unsigned long)errors, (unsigned long)other);
                        test_run_info((unsigned char *)report);
                    }
                }
                else
                {
                    other++;
                    have_previous = 0;
                }
            }
        }
        else
        {
            other++;
            /* Retain the last verified successful frame across PHY errors. */
        }
    }
}
#endif
void build_examples(void)
{
#if CRC_PROBE_ROLE_TX
    example_pointer = probe_tx;
#else
    example_pointer = probe_rx;
#endif
}
