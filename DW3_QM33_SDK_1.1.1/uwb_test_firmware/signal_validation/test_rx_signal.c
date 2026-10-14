#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#define RX_QUEUE_BARRIER() _ReadWriteBarrier()
#else
#define RX_QUEUE_BARRIER() __sync_synchronize()
#endif
#include "../../Drivers/API/Src/examples/ex_02a_simple_rx/rx_frame_queue.h"

static dwt_cirdiags_t mock_diag;
static uint8_t mock_dgc = 3;
static int allow_hardware;
static unsigned hardware_reads;
static char last_line[384];
static unsigned lines;
static unsigned log_mask;
static rx_frame_queue_t queue;

void test_run_info(unsigned char *data)
{
    assert(strlen((char *)data) < sizeof(last_line) - 1);
    strcpy(last_line, (char *)data);
    lines++;
}
void dwt_configciadiag(uint8_t mask) { log_mask = mask; }
uint8_t dwt_get_dgcdecision(void)
{
    assert(allow_hardware);
    hardware_reads++;
    return mock_dgc;
}
uint32_t dwt_read_reg(uint32_t address)
{
    assert(allow_hardware);
    hardware_reads++;
    switch (address)
    {
    case IP_DIAG_1_ID: return mock_diag.power;
    case IP_DIAG_2_ID: return mock_diag.F1;
    case IP_DIAG_3_ID: return mock_diag.F2;
    case IP_DIAG_4_ID: return mock_diag.F3;
    case IP_DIAG_8_ID: return mock_diag.FpIndex;
    case IP_DIAG_12_ID: return mock_diag.accumCount;
    default: assert(0); return 0;
    }
}

static rx_signal_snapshot_t capture(int ready)
{
    rx_signal_snapshot_t snapshot;
    allow_hardware = 1;
    rx_signal_capture(&snapshot, ready);
    allow_hardware = 0;
    return snapshot;
}

static void test_queue(void)
{
    unsigned i;
    rx_frame_record_t *write;
    const rx_frame_record_t *read;
    rx_frame_queue_init(&queue);
    assert(rx_frame_queue_peek(&queue) == NULL);
    write = rx_frame_queue_reserve(&queue);
    write->sequence = 77;
    /* An interrupt producer has only partially filled this slot. */
    assert(rx_frame_queue_peek(&queue) == NULL);
    write->reception_id = 123;
    memset(write->frame, 0xA5, sizeof(write->frame));
    write->signal.diag.power = 500;
    write->signal.dgc = 2;
    rx_frame_queue_commit(&queue);
    read = rx_frame_queue_peek(&queue);
    assert(read->sequence == 77 && read->reception_id == 123);
    for (i = 1; i < TEST_RX_QUEUE_SIZE; i++)
    {
        write = rx_frame_queue_reserve(&queue);
        assert(write != NULL && write != read);
        write->sequence = i;
        rx_frame_queue_commit(&queue);
    }
    /* Main is preempted while still processing the first slot. Do not overwrite. */
    for (i = 0; i < 100; i++) { assert(rx_frame_queue_reserve(&queue) == NULL); }
    assert(read->sequence == 77 && read->frame[31] == 0xA5);
    assert(read->signal.diag.power == 500 && read->signal.dgc == 2);
    rx_frame_queue_release(&queue);
    write = rx_frame_queue_reserve(&queue);
    assert(write == &queue.records[0]);
    write->sequence = 999;
    rx_frame_queue_commit(&queue);
    for (i = 1; i < TEST_RX_QUEUE_SIZE; i++)
    {
        assert(rx_frame_queue_peek(&queue)->sequence == i);
        rx_frame_queue_release(&queue);
    }
    assert(rx_frame_queue_peek(&queue)->sequence == 999);
    rx_frame_queue_release(&queue);
    assert(rx_frame_queue_peek(&queue) == NULL);
    /* Exercise unsigned index wrap and duplicate/out-of-order sequence values. */
    queue.head = queue.tail = UINT32_MAX - 8U;
    for (i = 0; i < 10000; i++)
    {
        write = rx_frame_queue_reserve(&queue);
        write->sequence = (i % 3 == 0) ? 42 : (10000U - i);
        write->reception_id = i;
        rx_frame_queue_commit(&queue);
        read = rx_frame_queue_peek(&queue);
        assert(read->reception_id == i);
        assert(read->sequence == ((i % 3 == 0) ? 42 : (10000U - i)));
        rx_frame_queue_release(&queue);
    }
    /* Aborting a reservation must not publish it. */
    write = rx_frame_queue_reserve(&queue);
    write->sequence = 111;
    assert(rx_frame_queue_peek(&queue) == NULL);
    assert(rx_frame_queue_reserve(&queue) == write);
}

/* Backlogged frames have different powers, gain states and duplicate/out-of-
 * order sequences. Simulate new live radio diagnostics before consuming each
 * batch; verify every queued result against reception_id, not the live state. */
static void test_snapshot_queue(void)
{
    unsigned batch, i;
    char report[384];
    rx_signal_init(DWT_DW3000_DEV_ID, 9);
    rx_frame_queue_init(&queue);
    for (batch = 0; batch < 100; batch++)
    {
        for (i = 0; i < TEST_RX_QUEUE_SIZE; i++)
        {
            uint32_t id = batch * TEST_RX_QUEUE_SIZE + i;
            rx_frame_record_t *record = rx_frame_queue_reserve(&queue);
            assert(record);
            mock_diag.power = 64U + id % 321U;
            mock_diag.F1 = 4000; mock_diag.F2 = 3600; mock_diag.F3 = 3200;
            mock_diag.accumCount = 128;
            mock_dgc = (uint8_t)(id % 8U);
            record->reception_id = id;
            record->sequence = id % 5U == 0U ? 7U : 100000U - id;
            memset(record->frame, (uint8_t)id, sizeof(record->frame));
            record->signal = capture(1);
            rx_frame_queue_commit(&queue);
        }
        assert(rx_frame_queue_reserve(&queue) == NULL);
        mock_diag.power = 9999;
        mock_dgc = 7;
        for (i = 0; i < TEST_RX_QUEUE_SIZE; i++)
        {
            uint32_t id = batch * TEST_RX_QUEUE_SIZE + i;
            uint32_t history_slot = rx_signal_history_next;
            const rx_frame_record_t *record = rx_frame_queue_peek(&queue);
            int16_t expected = rsl_calculate_signal_power(64U + id % 321U,
                                                         21, 128, (uint8_t)(id % 8U), 9, false);
            assert(record && record->reception_id == id);
            assert(record->sequence == (id % 5U == 0U ? 7U : 100000U - id));
            assert(record->frame[31] == (uint8_t)id);
            rx_signal_process(record->sequence, record->reception_id, id + 1,
                              &record->signal, report, sizeof(report));
            assert(rx_signal_history[history_slot].rssi_q8 == expected);
            assert(rx_signal_history[history_slot].sequence == record->sequence);
            assert(rx_signal_history[history_slot].reception_id == id);
            assert(rx_signal_history[history_slot].dgc == id % 8U);
            rx_frame_queue_release(&queue);
        }
    }
    assert(rx_signal_stats.samples == 100U * TEST_RX_QUEUE_SIZE);
    mock_dgc = 3;
}

int main(void)
{
    char report[384], value[16];
    unsigned i, reads;
    int16_t baseline;
    rx_signal_snapshot_t snapshot;
    test_queue();
    test_snapshot_queue();
    rx_signal_format_q8(value, sizeof(value), -1);
    assert(strcmp(value, "-0.00") == 0);
    rx_signal_format_q8(value, sizeof(value), -128);
    assert(strcmp(value, "-0.50") == 0);
    rx_signal_format_q8(value, sizeof(value), -20672);
    assert(strcmp(value, "-80.75") == 0);
    rx_signal_format_q8(value, sizeof(value), 65535);
    assert(strcmp(value, "256.00") == 0);

    mock_diag.power = 500; mock_diag.accumCount = 128;
    mock_diag.F1 = 4000; mock_diag.F2 = 3600; mock_diag.F3 = 3200;
    mock_diag.FpIndex = 64 * 12 + 32;
    /* SDK fixed-point math against the manual formula, both devices and PRFs. */
    for (i = 0; i < 4; i++)
    {
        unsigned factor = i < 2 ? 21 : 17;
        unsigned pcode = (i & 1) ? 3 : 9;
        double alpha = pcode == 9 ? 121.7 : 113.8;
        double expected = 10 * log10(500.0 * pow(2.0, factor) / (128.0 * 128.0)) + 18 - alpha;
        rx_signal_init(i < 2 ? DWT_DW3000_DEV_ID : DWT_QM33120_PDOA_DEV_ID, (uint8_t)pcode);
        snapshot = capture(1);
        rx_signal_process(42, i, 1, &snapshot, report, sizeof(report));
        assert(fabs(rx_signal_history[0].rssi_q8 / 256.0 - expected) < 0.15);
        expected = 10 * log10((1000.0*1000.0 + 900.0*900.0 + 800.0*800.0) / (128.0*128.0)) + 18 - alpha;
        assert(fabs(rx_signal_history[0].fp_power_q8 / 256.0 - expected) < 0.15);
    }
    rx_signal_init(DWT_DW3000_DEV_ID, 9);
    assert(log_mask == DW_CIA_DIAG_LOG_ALL);
    rx_signal_report("RX_SIGNAL_COMPLETE", report, sizeof(report));
    assert(strstr(last_line, "samples=0 invalid=0 rssi_avg_dbm=NA"));
    snapshot = capture(1);
    baseline = rsl_calculate_signal_power(500, 21, 128, 3, 9, false);
    reads = hardware_reads;
    /* Simulate a new radio reception before the old snapshot is processed. */
    mock_diag.power = 1000;
    mock_dgc = 7;
    rx_signal_process(0, 10, 1, &snapshot, report, sizeof(report));
    assert(hardware_reads == reads);
    assert(rx_signal_stats.samples == 1);
    assert(rx_signal_history[0].sequence == 0 && rx_signal_history[0].reception_id == 10);
    assert(rx_signal_history[0].diag.power == 500 && rx_signal_history[0].dgc == 3);
    assert(rx_signal_history[0].rssi_q8 == baseline);
#if TEST_SIGNAL_LOG_EVERY > 0
    assert(strstr(last_line, "fp_index=12.50 fp_index_q6=800 f1_raw=4000"));
    assert(strstr(last_line, "reception_id=10"));
#endif
    mock_dgc = 3;
    snapshot = capture(1);
    rx_signal_process(1, 11, 2, &snapshot, report, sizeof(report));
    assert(rx_signal_stats.rssi_max_q8 > baseline);
    assert(rx_signal_stats.rssi_min_q8 == baseline);
    mock_diag.accumCount = 0;
    snapshot = capture(1);
    rx_signal_process(2, 12, 3, &snapshot, report, sizeof(report));
    mock_diag.accumCount = 128;
    snapshot = capture(0); /* CIA unfinished/error: never borrow old diagnostics. */
    rx_signal_process(3, 13, 4, &snapshot, report, sizeof(report));
    mock_diag.power = 0;
    snapshot = capture(1);
    rx_signal_process(4, 14, 5, &snapshot, report, sizeof(report));
    mock_diag.power = 500;
    mock_diag.F1 = mock_diag.F2 = mock_diag.F3 = 0;
    snapshot = capture(1);
    rx_signal_process(5, 15, 6, &snapshot, report, sizeof(report));
    assert(rx_signal_stats.samples == 2 && rx_signal_stats.invalid == 4);
    assert(!rx_signal_history[5].valid && rx_signal_history[5].rssi_q8 == SHRT_MIN);

    mock_diag.F1 = 4000; mock_diag.F2 = 3600; mock_diag.F3 = 3200;
    rx_signal_init(DWT_DW3000_DEV_ID, 9);
    snapshot = capture(1);
    lines = 0;
    for (i = 0; i < 300000; i++)
    {
        rx_signal_process(i, i + 1, i + 1, &snapshot, report, sizeof(report));
    }
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
    puts(last_line);
    puts("PASS: queue ownership/full/wrap, immutable sequence+DGC snapshot, no background SPI, SDK power formulas, invalid diagnostics, 300000-frame aggregates");
    return 0;
}
