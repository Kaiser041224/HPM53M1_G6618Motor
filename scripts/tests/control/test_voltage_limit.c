/** @file test_voltage_limit.c
 * @brief 电压范围逐级放宽及统计测试
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_util.h"
#include "app_foc.h"
#include "app_foc_current.h"
#include "app_software_params.h"
extern void mock_params_reset(void);
void test_voltage_limit(void) {
    mock_params_reset(); app_foc_init();
    CHECK_NEAR(app_foc_current_duty_max(),0.885f,1e-6f); /* 新默认ceiling .95仍受软件域约束 */
    CHECK(app_foc_set_duty_ceiling(0.75f)==0);
    CHECK(app_foc_set_duty_ceiling(0.981f)!=0);
    CHECK(app_foc_set_duty_ceiling(NAN)!=0);
    CHECK(app_foc_enable()==0);
    CHECK(app_foc_set_duty_ceiling(0.8f)!=0);
    CHECK(app_foc_current_run_fresh(0,0,10,0,0,0,0,24,NULL,NULL)==0);
    CHECK_NEAR(app_foc_current_duty_max(),0.75f,1e-6f);
    for(unsigned i=0;i<500;++i)
        app_foc_current_run_fresh(0,0,10,0,0,0,0,24,NULL,NULL);
    CHECK(g_foc_current_snapshot.voltage_max_v>6.9f);
    CHECK(g_foc_current_snapshot.voltage_utilization>0.99f);
    CHECK(g_foc_current_snapshot.saturated_count>0);
    app_foc_disable();
    CHECK(app_foc_set_duty_ceiling(0.98f)==0);
    CHECK_NEAR(app_foc_current_duty_max(),0.885f,1e-6f); /* 配置限制仍保留 */
    app_software_params_mutable()->control.limits.duty_max=0.98f;
    CHECK(app_foc_set_duty_ceiling(0.98f)==0);
    CHECK_NEAR(app_foc_current_duty_max(),0.98f,1e-6f);
    CHECK(app_foc_set_duty_ceiling(0.70f)==0);
}
