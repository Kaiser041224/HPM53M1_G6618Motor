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

int main(void) {
    test_adc_pmt_queue_tail_duplicate();
    test_adc_pmt_slot_read_mapping();
    test_adc_seq_vbus_first_and_last();
    test_adc_trigger_fanout();
    test_adc_vbus_fast_latch();
    test_adc_vbus_fast_stale();
    test_adc1_no_seq_interrupt();
    test_adc1_status_cleared_at_1khz();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}