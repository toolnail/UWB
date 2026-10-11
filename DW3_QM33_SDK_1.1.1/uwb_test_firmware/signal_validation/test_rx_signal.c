#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../Drivers/API/Src/examples/ex_02a_simple_rx/rx_signal_metrics.h"
#include <deca_rsl.h>

static dwt_cirdiags_t mock_diag;
static int mock_read_error;
static int mock_calc_error;
static int mock_sentinel;
static char last_line[384];
static unsigned lines;
static unsigned log_mask;

void test_run_info(unsigned char *data)
{
    assert(strlen((char *)data) < sizeof(last_line) - 1);
    strcpy(last_line, (char *)data);
    lines++;
}
void dwt_configciadiag(uint8_t mask) { log_mask = mask; }
uint8_t dwt_get_dgcdecision(void) { return 3; }
int dwt_readdiagnostics_acc(dwt_cirdiags_t *diag, dwt_acc_idx_e acc)
{
    assert(acc == DWT_ACC_IDX_IP_M);
    *diag = mock_diag;
    return mock_read_error ? DWT_ERROR : DWT_SUCCESS;
}
int dwt_calculate_rssi(const dwt_cirdiags_t *diag, dwt_acc_idx_e acc, int16_t *out)
{
    assert(acc == DWT_ACC_IDX_IP_M);
    *out = mock_sentinel ? SHRT_MIN : rsl_calculate_signal_power(diag->power, 21, diag->accumCount, 3, 9, false);
    return mock_calc_error ? DWT_ERROR : DWT_SUCCESS;
}
int dwt_calculate_first_path_power(const dwt_cirdiags_t *diag, dwt_acc_idx_e acc, int16_t *out)
{
    assert(acc == DWT_ACC_IDX_IP_M);
    *out = rsl_calculate_first_path_power(diag->F1, diag->F2, diag->F3, diag->accumCount, 3, 9, false);
    return DWT_SUCCESS;
}

int main(void)
{
    char report[384], value[16];
    unsigned i;
    int16_t baseline;
    rx_signal_format_q8(value, sizeof(value), -1);
    assert(strcmp(value, "-0.00") == 0);
    rx_signal_format_q8(value, sizeof(value), -128);
    assert(strcmp(value, "-0.50") == 0);
    rx_signal_format_q8(value, sizeof(value), -20672);
    assert(strcmp(value, "-80.75") == 0);
    rx_signal_format_q8(value, sizeof(value), 65535);
    assert(strcmp(value, "256.00") == 0);

    /* Compare the actual SDK power math against the manual's log10 formula
     * for both PRFs and both hardware quantization factors. */
    for (i = 0; i < 4; i++)
    {
        unsigned factor = i < 2 ? 21 : 17;
        unsigned pcode = (i & 1) ? 3 : 9;
        double alpha = pcode == 9 ? 121.7 : 113.8;
        double expected = 10 * log10(500.0 * pow(2.0, factor) / (128.0 * 128.0)) + 18 - alpha;
        int16_t actual = rsl_calculate_signal_power(500, factor, 128, 3, (uint8_t)pcode, false);
        assert(fabs(actual / 256.0 - expected) < 0.15);
        expected = 10 * log10((1000.0*1000.0 + 900.0*900.0 + 800.0*800.0) / (128.0*128.0)) + 18 - alpha;
        actual = rsl_calculate_first_path_power(4000, 3600, 3200, 128, 3, (uint8_t)pcode, false);
        assert(fabs(actual / 256.0 - expected) < 0.15);
    }
    assert(((0x0908U & 0x1f00U) >> 8U) == 9U);

    rx_signal_init();
    assert(log_mask == DW_CIA_DIAG_LOG_ALL);
    rx_signal_report("RX_SIGNAL_COMPLETE", report, sizeof(report));
    assert(strstr(last_line, "samples=0 invalid=0 rssi_avg_dbm=NA"));
    mock_diag.power = 500; mock_diag.accumCount = 128;
    mock_diag.F1 = 4000; mock_diag.F2 = 3600; mock_diag.F3 = 3200;
    mock_diag.FpIndex = 64 * 12 + 32;
    rx_signal_measure(0, 1, report, sizeof(report));
    assert(rx_signal_stats.samples == 1);
    assert(rx_signal_history[0].sequence == 0 && rx_signal_history[0].valid);
#if TEST_SIGNAL_LOG_EVERY > 0
    assert(strstr(last_line, "fp_index=12.50 fp_index_q6=800 f1_raw=4000"));
#endif
    baseline = rx_signal_stats.rssi_min_q8;
    mock_diag.power = 1000;
    rx_signal_measure(1, 2, report, sizeof(report));
    assert(rx_signal_stats.rssi_max_q8 > baseline);
    assert(rx_signal_stats.rssi_min_q8 == baseline);
    assert(rx_signal_stats.rssi_sum_q8 == (int64_t)baseline + rx_signal_stats.rssi_max_q8);

    mock_diag.accumCount = 0; rx_signal_measure(2, 3, report, sizeof(report));
    mock_diag.accumCount = 128; mock_read_error = 1; rx_signal_measure(3, 4, report, sizeof(report));
    mock_read_error = 0; mock_calc_error = 1; rx_signal_measure(4, 5, report, sizeof(report));
    mock_calc_error = 0; mock_sentinel = 1; rx_signal_measure(5, 6, report, sizeof(report));
    mock_sentinel = 0;
    assert(rx_signal_stats.samples == 2 && rx_signal_stats.invalid == 4);
    assert(!rx_signal_history[5].valid);

    /* A full 300000-frame run exceeds a signed 32-bit sum at UWB levels. */
    rx_signal_init();
    mock_diag.power = 500;
    lines = 0;
    for (i = 0; i < 300000; i++) { rx_signal_measure(i, i + 1, report, sizeof(report)); }
    assert(rx_signal_stats.samples == 300000 && rx_signal_stats.invalid == 0);
    assert(rx_signal_stats.rssi_sum_q8 == (int64_t)baseline * 300000);
    assert(rx_signal_stats.rssi_sum_q8 < INT_MIN);
    assert(rx_signal_history_next == 300000U % TEST_SIGNAL_HISTORY_SIZE);
    assert(rx_signal_history[(299999U % TEST_SIGNAL_HISTORY_SIZE)].sequence == 299999);
#if TEST_SIGNAL_LOG_EVERY > 0
    assert(lines == 1U + 300000U / TEST_SIGNAL_LOG_EVERY - 1U / TEST_SIGNAL_LOG_EVERY);
#else
    assert(lines == 0);
#endif
    rx_signal_report("RX_SIGNAL_COMPLETE", report, sizeof(report));
    assert(strstr(last_line, "samples=300000 invalid=0"));
    puts(last_line);
    puts("PASS: SDK power formulas, signed formatting, invalid diagnostics, extrema, 300000-frame sums, history wrap, logging");
    return 0;
}
