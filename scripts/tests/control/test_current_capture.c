/**
 * @file test_current_capture.c
 * @brief 真实电流执行端口捕获及临时PI测试
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_foc.h"
#include "app_foc_current.h"
#include "test_util.h"
extern void mock_params_reset(void);
void test_current_capture(void) {
    mock_params_reset();
    app_foc_init();
    CHECK(app_foc_calib_capture(0.4f, true) != 0);
    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_calib_begin() == 0);
    CHECK(app_foc_calib_trial_pi(0.7f, 200) == 0);
    CHECK(app_foc_current_vtest_start(0.5f,0,1)!=0);
    CHECK(app_foc_calib_capture(0.4f, true) == 0);
    CHECK(app_foc_calib_capture(0.4f, true) != 0);
    for (unsigned i = 0; i < APP_FOC_IDENTIFY_SAMPLES; ++i)
        CHECK(app_foc_current_run_fresh(0, 0, 2, 0, 0, 0, 0, 24, NULL, NULL) == 0);
    uint16_t count;
    const id_rl_sample_t* data = app_foc_current_identify_data(&count);
    CHECK(count == APP_FOC_IDENTIFY_SAMPLES);
    CHECK_NEAR(data[10].v_d_previous_v, 0.4f, 1e-5f);
    CHECK_NEAR(g_foc_current_snapshot.v_d_v, 0.4f, 1e-5f);
    CHECK(app_foc_current_run_fresh(0, 0, 2, 0, 0, 0, 0, 24, NULL, NULL) == 0);
    CHECK(g_foc_current_snapshot.v_d_v > 1.0f);            /* 脉冲自动结束，回到临时PI */
    app_foc_calib_end();
    CHECK(app_foc_current_run_fresh(0, 0, 2, 0, 0, 0, 0, 24, NULL, NULL) == 0);
    CHECK_NEAR(g_foc_current_snapshot.v_d_v, 0.6f, 1e-5f); /* 正式PI恢复 */
    app_foc_current_identify_data(&count);
    CHECK(count == 0);
    CHECK(app_foc_calib_begin() == 0);
    CHECK(app_foc_calib_voltage(0.6f, 1000) == 0);
    CHECK(app_foc_calib_pulse(0.3f, 4) == 0);
    for (unsigned i = 0; i < 128; ++i)
        app_foc_current_run_fresh(0, 0, 3, 0, 0, 0, 0, 24, NULL, NULL);
    app_foc_current_identify_data(&count);
    CHECK(count == 128);
    CHECK(app_foc_current_run_fresh(0, 0, 3, 0, 0, 0, 0, 24, NULL, NULL) == 0);
    CHECK_NEAR(g_foc_current_snapshot.v_d_v, 0.6f, 1e-5f);
    CHECK(app_foc_calib_capture(0, false) == 0);
    for (unsigned i = 0; i < 128; ++i)
        app_foc_current_run_fresh(0, 0, 3, 0, 0, 0, 0, 24, NULL, NULL);
    CHECK_NEAR(g_foc_current_snapshot.v_d_v, 0.6f, 1e-5f);
    CHECK(app_foc_calib_capture(0.7f, true) == 0);
    for (unsigned i = 0; i < 129; ++i)
        app_foc_current_run_fresh(0, 0, 3, 0, 0, 0, 0, 24, NULL, NULL);
    CHECK_NEAR(g_foc_current_snapshot.v_d_v, 0.6f, 1e-5f); /* 脉冲返回恒压，不返回PI */
    app_foc_calib_end();
    CHECK(app_foc_calib_begin() == 0);
    CHECK(app_foc_calib_trial_pi(0.7f, 200) == 0);
    CHECK(app_foc_calib_capture(0.4f, true) == 0);
    app_foc_disable(); /* 正在捕获期间关闭，下一次运行不得残留脉冲 */
    app_foc_current_identify_data(&count);
    CHECK(count == 0);
    CHECK(app_foc_calib_trial_pi(0.7f, 200) != 0);
    CHECK(app_foc_enable()==0);
    CHECK(app_foc_calib_begin()==0);
    CHECK(app_foc_calib_capture(0,false)==0);
    for (unsigned i=0;i<128;++i)
        app_foc_current_run_fresh(0,0,0,0,0.3f,-0.1f,0.2f,24,NULL,NULL);
    data=app_foc_current_identify_data(&count);
    CHECK_NEAR(data[10].i_u_a,0.3f,1e-6f);
    CHECK_NEAR(data[10].i_v_a,-0.1f,1e-6f);
    CHECK_NEAR(data[10].i_w_a,0.2f,1e-6f); /* 不重建W，保留三相不平衡观测 */
    app_foc_calib_end();
    app_foc_disable();
    CHECK(app_foc_enable()==0);
    CHECK(app_foc_calib_begin()==0);
    CHECK(app_foc_calib_voltage(0.5f,1000)==0);
    CHECK(app_foc_calib_capture(0,false)==0);
    app_foc_current_run_fresh(0,0,0,0,0,0,0,24,NULL,NULL);
    CHECK(app_foc_current_run_fresh(0,0,0,0,0,0,0,0,NULL,NULL)!=0);
    data=app_foc_current_identify_data(&count);
    CHECK(count==APP_FOC_IDENTIFY_SAMPLES);
    CHECK(!data[0].valid); /* 禁止将失败帧后的数据拼成连续序列 */
    CHECK(app_foc_calib_capture(0.5f,true)==0);
    CHECK(app_foc_current_run_fresh(0,0,0,0,NAN,0,0,24,NULL,NULL)!=0);
    CHECK_NEAR(g_foc_current_snapshot.v_d_v,0,1e-6f);
    data=app_foc_current_identify_data(&count);
    CHECK(count==APP_FOC_IDENTIFY_SAMPLES && !data[0].valid);
    /* 无效捕获冻结后，即使后续恢复好帧也不可洗掉无效标记。 */
    for (unsigned i=0;i<140;++i)
        app_foc_current_run_fresh(0,0,0,0,0,0,0,24,NULL,NULL);
    data=app_foc_current_identify_data(&count);
    CHECK(count==APP_FOC_IDENTIFY_SAMPLES && !data[0].valid);
    app_foc_calib_end(); app_foc_disable();
}
