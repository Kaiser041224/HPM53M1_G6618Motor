/**
 * @file    test_foc_math.c
 * @brief   foc_math.h 用例（变换一致性 / 归一化边界 / √3 常数敏感项）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "foc_math.h"

void test_foc_math(void) {
    /* wrap_2pi：边界与负值 */
    CHECK_NEAR(foc_wrap_2pi(0.0f), 0.0f, 1e-6f);
    CHECK_NEAR(foc_wrap_2pi(FOC_TWO_PI_F), 0.0f, 1e-5f);
    CHECK_NEAR(foc_wrap_2pi(-0.5f), FOC_TWO_PI_F - 0.5f, 1e-5f);
    CHECK_NEAR(foc_wrap_2pi(FOC_TWO_PI_F * 3.5f), FOC_PI_F, 1e-4f);
    CHECK_NEAR(foc_wrap_2pi(1234.5f), 1234.5f - 196.0f * FOC_TWO_PI_F, 1e-3f);

    /* wrap_pm_pi：±π 归到 +π；π 邻域；2π×2.25 → π/2；大角度（θe 量级） */
    CHECK_NEAR(foc_wrap_pm_pi(FOC_PI_F), FOC_PI_F, 1e-5f);
    CHECK_NEAR(foc_wrap_pm_pi(-FOC_PI_F), FOC_PI_F, 1e-5f);
    CHECK_NEAR(foc_wrap_pm_pi(FOC_PI_F - 1e-4f), FOC_PI_F - 1e-4f, 1e-5f);
    CHECK_NEAR(foc_wrap_pm_pi(FOC_PI_F + 1e-4f), -FOC_PI_F + 1e-4f, 1e-5f);
    CHECK_NEAR(foc_wrap_pm_pi(FOC_TWO_PI_F * 2.25f), FOC_PI_F * 0.5f, 1e-4f);
    CHECK_NEAR(foc_wrap_pm_pi(1600.0f), -2.21218f, 1e-3f);

    /* Clarke：三相平衡 → α = U 相幅值、β = 0 */
    {
        float ia, ib;
        foc_clarke(3.0f, -1.5f, -1.5f, &ia, &ib);
        CHECK_NEAR(ia, 3.0f, 1e-5f);
        CHECK_NEAR(ib, 0.0f, 1e-5f);
    }

    /* Clarke：非平衡输入 → β 独立校验（对 √3 常数敏感） */
    {
        float ia, ib;
        foc_clarke(0.0f, 1.0f, 0.0f, &ia, &ib);
        CHECK_NEAR(ia, -1.0f / 3.0f, 1e-6f);
        CHECK_NEAR(ib, 0.5773502691896258f, 1e-6f);
    }

    /* Park / 反 Park 往返一致 */
    {
        float id, iq, va, vb;
        foc_park(2.5f, -1.25f, 0.7f, &id, &iq);
        foc_inv_park(id, iq, 0.7f, &va, &vb);
        CHECK_NEAR(va, 2.5f, 1e-5f);
        CHECK_NEAR(vb, -1.25f, 1e-5f);
    }

    /* Park：θ=0 时 id=iα、iq=iβ */
    {
        float id, iq;
        foc_park(1.5f, -0.75f, 0.0f, &id, &iq);
        CHECK_NEAR(id, 1.5f, 1e-6f);
        CHECK_NEAR(iq, -0.75f, 1e-6f);
    }

    /* 反 Park（预计算 s/c）：θ=π/2（s=1,c=0）→ va=−vq、vb=vd（独立校验） */
    {
        float va, vb;
        foc_inv_park_sc(2.0f, 3.0f, 1.0f, 0.0f, &va, &vb);
        CHECK_NEAR(va, -3.0f, 1e-6f);
        CHECK_NEAR(vb, 2.0f, 1e-6f);
    }

    /* 反 Clarke：三相和为零 */
    {
        float vu, vv, vw;
        foc_inv_clarke(1.0f, 0.5f, &vu, &vv, &vw);
        CHECK_NEAR(vu + vv + vw, 0.0f, 1e-5f);
    }

    /* 反 Clarke：已知矢量 → 独立期望值（对 √3 常数敏感） */
    {
        float vu, vv, vw;
        foc_inv_clarke(0.0f, 1.0f, &vu, &vv, &vw);
        CHECK_NEAR(vu, 0.0f, 1e-6f);
        CHECK_NEAR(vv, 0.8660254037844386f, 1e-6f);
        CHECK_NEAR(vw, -0.8660254037844386f, 1e-6f);
    }

    /* Clarke → 反 Clarke 往返（零和输入，含 β≠0） */
    {
        float ia, ib, vu, vv, vw;
        foc_clarke(1.0f, -2.0f, 1.0f, &ia, &ib);
        foc_inv_clarke(ia, ib, &vu, &vv, &vw);
        CHECK_NEAR(vu, 1.0f, 1e-5f);
        CHECK_NEAR(vv, -2.0f, 1e-5f);
        CHECK_NEAR(vw, 1.0f, 1e-5f);
    }

    /* _sc 变体与 sincos 版本一致 */
    {
        float s, c, id1, iq1, id2, iq2;
        foc_sincos(1.1f, &s, &c);
        foc_park(0.5f, -0.25f, 1.1f, &id1, &iq1);
        foc_park_sc(0.5f, -0.25f, s, c, &id2, &iq2);
        CHECK_NEAR(id2, id1, 1e-5f);
        CHECK_NEAR(iq2, iq1, 1e-5f);
    }

    /* 有限性判断（NAN/INFINITY 常量；-ffast-math 下位级判断仍有效） */
    CHECK(foc_finite(1.0f));
    CHECK(!foc_finite(NAN));
    CHECK(!foc_finite(INFINITY));
    CHECK(!foc_finite(-INFINITY));
}
