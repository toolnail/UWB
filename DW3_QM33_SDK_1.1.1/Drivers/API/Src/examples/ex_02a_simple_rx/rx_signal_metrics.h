/* RX signal diagnostics for the protocol-free packet delivery experiment. */
#ifndef RX_SIGNAL_METRICS_H
#define RX_SIGNAL_METRICS_H

#include <limits.h>
#include <deca_device_api.h>
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

#if TEST_SIGNAL_ENABLE
#if TEST_SIGNAL_HISTORY_SIZE < 1
#error TEST_SIGNAL_HISTORY_SIZE must be positive
#endif

typedef struct
{
    uint32_t sequence;
    dwt_cirdiags_t diag;
    int16_t rssi_q8;
    int16_t fp_power_q8;
    uint8_t dgc;
    uint8_t valid;
} rx_signal_record_t;

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

/* Keep signed fractions (including -0.xx) without float printf support. */
static void rx_signal_format_q8(char *text, size_t size, int32_t q8)
{
    uint32_t magnitude = (uint32_t)((q8 < 0) ? -q8 : q8);
    uint32_t hundredths = (magnitude * 100U + 128U) / 256U;
    snprintf(text, size, "%s%lu.%02lu", (q8 < 0) ? "-" : "",
             (unsigned long)(hundredths / 100U), (unsigned long)(hundredths % 100U));
}

static void rx_signal_init(void)
{
    memset((void *)rx_signal_history, 0, sizeof(rx_signal_history));
    memset(&rx_signal_stats, 0, sizeof(rx_signal_stats));
    rx_signal_history_next = 0U;
    /* Required before reading F1/F2/F3, power and accumulated symbols. */
    dwt_configciadiag(DW_CIA_DIAG_LOG_ALL);
}

/* Called only for a first, validated, in-range DATA sequence, while the
 * single-buffer receiver is idle and before the next dwt_rxenable(). */
static void rx_signal_measure(uint32_t sequence, uint32_t received, char *report, size_t size)
{
    rx_signal_record_t record = {0};
    int read_ok;
    int rssi_ok = DWT_ERROR;
    int fp_ok = DWT_ERROR;
    int print_sample = 0;

    record.sequence = sequence;
    read_ok = dwt_readdiagnostics_acc(&record.diag, DWT_ACC_IDX_IP_M);
    record.dgc = dwt_get_dgcdecision();
    record.rssi_q8 = SHRT_MIN;
    record.fp_power_q8 = SHRT_MIN;
    if ((read_ok == DWT_SUCCESS) && (record.diag.accumCount != 0U) &&
        (record.diag.power != 0U) &&
        ((record.diag.F1 >= 4U) || (record.diag.F2 >= 4U) || (record.diag.F3 >= 4U)))
    {
        rssi_ok = dwt_calculate_rssi(&record.diag, DWT_ACC_IDX_IP_M, &record.rssi_q8);
        fp_ok = dwt_calculate_first_path_power(&record.diag, DWT_ACC_IDX_IP_M, &record.fp_power_q8);
    }
    record.valid = (uint8_t)((rssi_ok == DWT_SUCCESS) && (fp_ok == DWT_SUCCESS) &&
                            (record.rssi_q8 != SHRT_MIN) && (record.fp_power_q8 != SHRT_MIN));
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
                 "RX_SIGNAL seq=%lu received=%lu diag_valid=%u rssi_dbm=%s fp_power_dbm=%s gap_db=%s fp_index=%s fp_index_q6=%u f1_raw=%lu f2_raw=%lu f3_raw=%lu cir_power=%lu accum_count=%u dgc=%u",
                 (unsigned long)sequence, (unsigned long)received, (unsigned int)record.valid,
                 rssi, fp, gap, index, (unsigned int)record.diag.FpIndex,
                 (unsigned long)record.diag.F1, (unsigned long)record.diag.F2,
                 (unsigned long)record.diag.F3, (unsigned long)record.diag.power,
                 (unsigned int)record.diag.accumCount, (unsigned int)record.dgc);
        test_run_info((unsigned char *)report);
    }
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
#define rx_signal_init() ((void)0)
#define rx_signal_measure(sequence, received, report, size) ((void)0)
#define rx_signal_report(prefix, report, size) ((void)0)
#endif
#endif
