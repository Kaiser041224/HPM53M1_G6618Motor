/**
 * @file    test_cogging_store.c
 * @brief   齿槽角表独立 flash 记录往返与 CRC 拒绝测试
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_cogging.h"
#include "app_cogging_store.h"
#include "test_util.h"

void mock_intf_flash_reset(void);
uint8_t* mock_intf_flash_data(void);

void test_cogging_store(void) {
    app_cogging_tables_t tables = {0};

    mock_intf_flash_reset();
    app_cogging_init();
    for (uint16_t i = 0U; i < ALGO_COGGING_ROTOR_POINTS; i++) {
        tables.rotor[i] = (int16_t)(i - 256);
    }
    for (uint16_t i = 0U; i < ALGO_COGGING_OUTPUT_POINTS; i++) {
        tables.output[i] = (int16_t)(i / 2);
    }
    tables.rotor_valid = true;
    tables.output_valid = true;
    tables.rotor_stats.rms_nm = 0.0123f;
    tables.rotor_stats.peak_to_peak_nm = 0.045f;
    tables.rotor_stats.valid_bins = ALGO_COGGING_ROTOR_POINTS;
    tables.rotor_stats.valid = true;
    tables.output_stats.rms_nm = 0.067f;
    tables.output_stats.peak_to_peak_nm = 0.12f;
    tables.output_stats.valid_bins = ALGO_COGGING_OUTPUT_POINTS;
    tables.output_stats.valid = true;

    CHECK(app_cogging_set_tables(&tables) == 0);
    CHECK(app_cogging_store_save() == 0);
    app_cogging_clear_tables();
    CHECK(app_cogging_store_load() == 0);
    const app_cogging_tables_t* active = app_cogging_get_tables();
    CHECK(active->rotor_valid);
    CHECK(active->output_valid);
    CHECK(active->rotor[0] == tables.rotor[0]);
    CHECK(active->rotor[ALGO_COGGING_ROTOR_POINTS - 1U]
          == tables.rotor[ALGO_COGGING_ROTOR_POINTS - 1U]);
    CHECK(active->output[ALGO_COGGING_OUTPUT_POINTS - 1U]
          == tables.output[ALGO_COGGING_OUTPUT_POINTS - 1U]);
    CHECK_NEAR(active->rotor_stats.rms_nm, tables.rotor_stats.rms_nm, 1e-6f);

    /* CRC/魔数破坏后必须拒绝，不污染活动表。 */
    uint8_t* raw = mock_intf_flash_data();
    raw[0] ^= 0x01U;
    app_cogging_clear_tables();
    CHECK(app_cogging_store_load() != 0);
    CHECK(!app_cogging_get_tables()->rotor_valid);
}
