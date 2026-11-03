/**
 * @file    test_main.c
 * @brief   ADC 采样链主机自测入口
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

void test_adc_pmt_queue_tail_duplicate(void);
void test_adc_pmt_slot_read_mapping(void);
void test_adc_seq_vbus_first_and_last(void);
void test_adc_trigger_fanout(void);
void test_adc_vbus_fast_latch(void);
void test_adc_vbus_fast_stale(void);
void test_adc1_no_seq_interrupt(void);
void test_adc1_status_cleared_at_1khz(void);
void test_can_id_decode_table(void);
void test_can_id_read_samples_median(void);
void test_can_id_read_rejects_no_data(void);
void test_can_id_read_tolerates_outlier(void);
void test_can_id_release_success(void);
void test_can_id_release_requires_first_frame(void);
void test_can_id_release_rolls_back_without_frame(void);

int main(void) {
    test_adc_pmt_queue_tail_duplicate();
    test_adc_pmt_slot_read_mapping();
    test_adc_seq_vbus_first_and_last();
    test_adc_trigger_fanout();
    test_adc_vbus_fast_latch();
    test_adc_vbus_fast_stale();
    test_adc1_no_seq_interrupt();
    test_adc1_status_cleared_at_1khz();
    test_can_id_decode_table();
    test_can_id_read_samples_median();
    test_can_id_read_rejects_no_data();
    test_can_id_read_tolerates_outlier();
    test_can_id_release_success();
    test_can_id_release_requires_first_frame();
    test_can_id_release_rolls_back_without_frame();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}