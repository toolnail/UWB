/* RX signal diagnostics for the protocol-free packet delivery experiment. */
#ifndef RX_SIGNAL_METRICS_H
#define RX_SIGNAL_METRICS_H

#include <limits.h>
#include <deca_device_api.h>
#include <deca_rsl.h>
#include "dw3000/dw3000_deca_regs.h"
#include <stdio.h>
#include <string.h>

extern void test_run_info(unsigned char *data);

#ifndef TEST_SIGNAL_ENABLE
#define TEST_SIGNAL_ENABLE 1
#endif
#ifndef TEST_SIGNAL_LOG_EVERY
#define TEST_SIGNAL_LOG_EVERY 1000UL
#endif
#ifndef TEST_SIGNAL_HISTORY_SIZE
#define TEST_SIGNAL_HISTORY_SIZE 64U
#endif

/* All hardware inputs belong to one reception and are captured BEFORE the next
 * RX command. Background processing must never read live diagnostic registers. */
typedef struct
{
    dwt_cirdiags_t diag;
    uint8_t dgc;
    uint8_t ready;
} rx_signal_snapshot_t;

typedef struct
{
    uint32_t sequence;
    uint32_t reception_id;
    dwt_cirdiags_t diag;
    int16_t rssi_q8;
    int16_t fp_power_q8;
    uint8_t dgc;
    uint8_t valid;
} rx_signal_record_t;

#if TEST_SIGNAL_ENABLE
#if TEST_SIGNAL_HISTORY_SIZE < 1
#error TEST_SIGNAL_HISTORY_SIZE must be positive
#endif

typedef struct
{
    uint32_t samples;
    uint32_t invalid;
    int64_t rssi_sum_q8;
    int64_t fp_sum_q8;
    int16_t rssi_min_q8;
    int16_t rssi_max_q8;
    int16_t fp_min_q8;
    int16_t fp_max_q8;
} rx_signal_stats_t;

/* Bounded RAM history for inspection at a debugger breakpoint. It wraps;
 * rx_signal_history_next is the slot for the next valid DATA reception.
 * Full-run aggregates live in rx_signal_stats, even when RTT lines are skipped. */
static volatile rx_signal_record_t rx_signal_history[TEST_SIGNAL_HISTORY_SIZE];
static volatile uint32_t rx_signal_history_next;
static rx_signal_stats_t rx_signal_stats;
static uint8_t rx_signal_quantization;
static uint8_t rx_signal_pcode;
static uint32_t rx_signal_power_mask;

/* Keep signed fractions (including -0.xx) without float printf support. */
static void rx_signal_format_q8(char *text, size_t size, int32_t q8)
{
    uint32_t magnitude = (uint32_t)((q8 < 0) ? -q8 : q8);
    uint32_t hundredths = (magnitude * 100U + 128U) / 256U;
    snprintf(text, size, "%s%lu.%02lu", (q8 < 0) ? "-" : "",
             (unsigned long)(hundredths / 100U), (unsigned long)(hundredths % 100U));
}

static void rx_signal_init(uint32_t device_id, uint8_t rx_pcode)
{
    memset((void *)rx_signal_history, 0, sizeof(rx_signal_history));
    memset(&rx_signal_stats, 0, sizeof(rx_signal_stats));
    rx_signal_history_next = 0U;
    rx_signal_pcode = rx_pcode;
    rx_signal_quantization = ((device_id == DWT_DW3000_DEV_ID) ||
                             (device_id == DWT_DW3000_PDOA_DEV_ID)) ? 21U : 17U;
    rx_signal_power_mask = (rx_signal_quantization == 21U) ? 0x1FFFFUL : 0xFFFFFUL;
    /* Required before reading F1/F2/F3, power and accumulated symbols. */
    dwt_configciadiag(DW_CIA_DIAG_LOG_ALL);
}

/* IP_DIAG addresses/layout below are identical on DW3000 and QM33; power width
 * differs. Read only the six required Ipatov words (24 bytes), not both 108-byte
 * Ipatov/STS blocks in dwt_readdiagnostics_acc(). Manual RX keeps these live
 * registers and DGC stable until this snapshot is finished. */
static void rx_signal_capture(rx_signal_snapshot_t *snapshot, int cia_ready)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (!cia_ready) { return; }
    snapshot->diag.power = dwt_read_reg(IP_DIAG_1_ID) & rx_signal_power_mask;
    snapshot->diag.F1 = dwt_read_reg(IP_DIAG_2_ID) & IP_DIAG_2_IPF1_BIT_MASK;
    snapshot->diag.F2 = dwt_read_reg(IP_DIAG_3_ID) & IP_DIAG_3_IPF2_BIT_MASK;
    snapshot->diag.F3 = dwt_read_reg(IP_DIAG_4_ID) & IP_DIAG_4_IPF3_BIT_MASK;
    snapshot->diag.FpIndex = (uint16_t)(dwt_read_reg(IP_DIAG_8_ID) & IP_DIAG_8_IPFPLOC_BIT_MASK);
    snapshot->diag.accumCount = (uint16_t)(dwt_read_reg(IP_DIAG_12_ID) & IP_DIAG_12_IPNACC_BIT_MASK);
    snapshot->dgc = dwt_get_dgcdecision();
    snapshot->ready = 1U;
}

/* Shared by unique and duplicate DATA receptions. No statistics or text output. */
static rx_signal_record_t rx_signal_calculate(uint32_t sequence, uint32_t reception_id,
                                             const rx_signal_snapshot_t *snapshot)
{
    rx_signal_record_t record = {0};

    record.sequence = sequence;
    record.reception_id = reception_id;
    record.diag = snapshot->diag;
    record.dgc = snapshot->dgc;
    record.rssi_q8 = SHRT_MIN;
    record.fp_power_q8 = SHRT_MIN;
    if (snapshot->ready && (record.diag.accumCount != 0U) &&
        (record.diag.power != 0U) &&
        ((record.diag.F1 >= 4U) || (record.diag.F2 >= 4U) || (record.diag.F3 >= 4U)))
    {
        record.rssi_q8 = rsl_calculate_signal_power((int32_t)record.diag.power,
            rx_signal_quantization, record.diag.accumCount, record.dgc, rx_signal_pcode, false);
        record.fp_power_q8 = rsl_calculate_first_path_power(record.diag.F1, record.diag.F2,
            record.diag.F3, record.diag.accumCount, record.dgc, rx_signal_pcode, false);
    }
    record.valid = (uint8_t)((record.rssi_q8 != SHRT_MIN) && (record.fp_power_q8 != SHRT_MIN));
    return record;
}

/* Main-loop only. Uses saved inputs; no SPI and no current DGC lookup. */
static rx_signal_record_t rx_signal_process(uint32_t sequence, uint32_t reception_id, uint32_t received,
                              const rx_signal_snapshot_t *snapshot, char *report, size_t size)
{
    rx_signal_record_t record = rx_signal_calculate(sequence, reception_id, snapshot);
    int print_sample = 0;
    rx_signal_history[rx_signal_history_next] = record;
    rx_signal_history_next = (rx_signal_history_next + 1U) % TEST_SIGNAL_HISTORY_SIZE;

    if (record.valid)
    {
        if (rx_signal_stats.samples == 0U)
        {
            rx_signal_stats.rssi_min_q8 = rx_signal_stats.rssi_max_q8 = record.rssi_q8;
            rx_signal_stats.fp_min_q8 = rx_signal_stats.fp_max_q8 = record.fp_power_q8;
        }
        if (record.rssi_q8 < rx_signal_stats.rssi_min_q8) { rx_signal_stats.rssi_min_q8 = record.rssi_q8; }
        if (record.rssi_q8 > rx_signal_stats.rssi_max_q8) { rx_signal_stats.rssi_max_q8 = record.rssi_q8; }
        if (record.fp_power_q8 < rx_signal_stats.fp_min_q8) { rx_signal_stats.fp_min_q8 = record.fp_power_q8; }
        if (record.fp_power_q8 > rx_signal_stats.fp_max_q8) { rx_signal_stats.fp_max_q8 = record.fp_power_q8; }
        rx_signal_stats.samples++;
        rx_signal_stats.rssi_sum_q8 += record.rssi_q8;
        rx_signal_stats.fp_sum_q8 += record.fp_power_q8;
    }
    else
    {
        rx_signal_stats.invalid++;
    }

#if TEST_SIGNAL_LOG_EVERY > 0
    print_sample = (received == 1U) || ((received % TEST_SIGNAL_LOG_EVERY) == 0U);
#endif
    if (print_sample)
    {
        char rssi[16], fp[16], gap[16], index[16];
        if (record.valid)
        {
            rx_signal_format_q8(rssi, sizeof(rssi), record.rssi_q8);
            rx_signal_format_q8(fp, sizeof(fp), record.fp_power_q8);
            rx_signal_format_q8(gap, sizeof(gap), (int32_t)record.rssi_q8 - record.fp_power_q8);
        }
        else
        {
            strcpy(rssi, "NA"); strcpy(fp, "NA"); strcpy(gap, "NA");
        }
        rx_signal_format_q8(index, sizeof(index), (int32_t)record.diag.FpIndex * 4);
        snprintf(report, size,
                 "RX_SIGNAL seq=%lu received=%lu diag_valid=%u rssi_dbm=%s fp_power_dbm=%s gap_db=%s fp_index=%s fp_index_q6=%u f1_raw=%lu f2_raw=%lu f3_raw=%lu cir_power=%lu accum_count=%u dgc=%u reception_id=%lu",
                 (unsigned long)sequence, (unsigned long)received, (unsigned int)record.valid,
                 rssi, fp, gap, index, (unsigned int)record.diag.FpIndex,
                 (unsigned long)record.diag.F1, (unsigned long)record.diag.F2,
                 (unsigned long)record.diag.F3, (unsigned long)record.diag.power,
                 (unsigned int)record.diag.accumCount, (unsigned int)record.dgc,
                 (unsigned long)record.reception_id);
        test_run_info((unsigned char *)report);
    }
    return record;
}

static void rx_signal_report(const char *prefix, char *report, size_t size)
{
    char rssi_avg[16], rssi_min[16], rssi_max[16];
    char fp_avg[16], fp_min[16], fp_max[16], gap[16];
    if (rx_signal_stats.samples == 0U)
    {
        snprintf(report, size, "%s samples=0 invalid=%lu rssi_avg_dbm=NA rssi_min_dbm=NA rssi_max_dbm=NA fp_avg_dbm=NA fp_min_dbm=NA fp_max_dbm=NA gap_avg_db=NA",
                 prefix, (unsigned long)rx_signal_stats.invalid);
    }
    else
    {
        int64_t count = rx_signal_stats.samples;
        rx_signal_format_q8(rssi_avg, sizeof(rssi_avg), (int32_t)(rx_signal_stats.rssi_sum_q8 / count));
        rx_signal_format_q8(rssi_min, sizeof(rssi_min), rx_signal_stats.rssi_min_q8);
        rx_signal_format_q8(rssi_max, sizeof(rssi_max), rx_signal_stats.rssi_max_q8);
        rx_signal_format_q8(fp_avg, sizeof(fp_avg), (int32_t)(rx_signal_stats.fp_sum_q8 / count));
        rx_signal_format_q8(fp_min, sizeof(fp_min), rx_signal_stats.fp_min_q8);
        rx_signal_format_q8(fp_max, sizeof(fp_max), rx_signal_stats.fp_max_q8);
        rx_signal_format_q8(gap, sizeof(gap), (int32_t)((rx_signal_stats.rssi_sum_q8 - rx_signal_stats.fp_sum_q8) / count));
        snprintf(report, size,
                 "%s samples=%lu invalid=%lu rssi_avg_dbm=%s rssi_min_dbm=%s rssi_max_dbm=%s fp_avg_dbm=%s fp_min_dbm=%s fp_max_dbm=%s gap_avg_db=%s",
                 prefix, (unsigned long)rx_signal_stats.samples, (unsigned long)rx_signal_stats.invalid,
                 rssi_avg, rssi_min, rssi_max, fp_avg, fp_min, fp_max, gap);
    }
    test_run_info((unsigned char *)report);
}
#else
#define rx_signal_init(device_id, rx_pcode) ((void)0)
#define rx_signal_capture(snapshot, cia_ready) memset((snapshot), 0, sizeof(*(snapshot)))
static rx_signal_record_t rx_signal_calculate(uint32_t sequence, uint32_t reception_id,
                                             const rx_signal_snapshot_t *snapshot)
{
    rx_signal_record_t record = {0};
    (void)snapshot;
    record.sequence = sequence; record.reception_id = reception_id;
    record.rssi_q8 = record.fp_power_q8 = SHRT_MIN;
    return record;
}
#define rx_signal_process(sequence, reception_id, received, snapshot, report, size) \
    rx_signal_calculate(sequence, reception_id, snapshot)
#define rx_signal_report(prefix, report, size) ((void)0)
#endif
#endif
