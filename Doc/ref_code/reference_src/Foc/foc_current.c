#include "foc_current.h"

#include "config.h"

void Foc_CurrentSetVbus(Foc_Current_t *cur, float vbus_v)
{
  if (vbus_v < CFG_VBUS_GAIN_MIN_V)
  {
    vbus_v = CFG_VBUS_NOMINAL_V;
  }
  cur->kp = cur->kp_v / vbus_v;
  cur->ki = cur->ki_v / vbus_v;
}

void Foc_CurrentInit(Foc_Current_t *cur, float r_ohm, float l_h, float bw_hz,
                     float vbus_v, float i_lim, float v_lim)
{
  const float wc = FOC_TWO_PI * bw_hz;

  cur->ia = 0.0f;
  cur->ib = 0.0f;
  cur->id = 0.0f;
  cur->iq = 0.0f;
  cur->vd = 0.0f;
  cur->vq = 0.0f;
  cur->id_i = 0.0f;
  cur->iq_i = 0.0f;
  cur->kp_v = l_h * wc;
  cur->ki_v = r_ohm * wc;
  cur->i_lim = i_lim;
  cur->v_lim = v_lim;
  Foc_CurrentSetVbus(cur, vbus_v);
}

void Foc_CurrentReset(Foc_Current_t *cur)
{
  cur->vd = 0.0f;
  cur->vq = 0.0f;
  cur->id_i = 0.0f;
  cur->iq_i = 0.0f;
}

void Foc_CurrentStep(Foc_Current_t *cur, float ia, float ib, float elec_rad,
                     float id_ref, float iq_ref, float dt_s, uint8_t run_pi)
{
  Foc_Dq_t dq;
  float id_err;
  float iq_err;

  cur->ia = ia;
  cur->ib = ib;
  dq = Foc_Park(Foc_Clarke(ia, ib), elec_rad);
  cur->id = dq.d;
  cur->iq = dq.q;

  if ((run_pi == 0U) || (dt_s <= 0.0f))
  {
    Foc_CurrentReset(cur);
    return;
  }

  id_ref = Foc_Clamp(id_ref, -cur->i_lim, cur->i_lim);
  iq_ref = Foc_Clamp(iq_ref, -cur->i_lim, cur->i_lim);
  id_err = id_ref - cur->id;
  iq_err = iq_ref - cur->iq;

  cur->id_i += cur->ki * id_err * dt_s;
  cur->iq_i += cur->ki * iq_err * dt_s;
  cur->id_i = Foc_Clamp(cur->id_i, -cur->v_lim, cur->v_lim);
  cur->iq_i = Foc_Clamp(cur->iq_i, -cur->v_lim, cur->v_lim);

  cur->vd = Foc_Clamp((cur->kp * id_err) + cur->id_i, -cur->v_lim, cur->v_lim);
  cur->vq = Foc_Clamp((cur->kp * iq_err) + cur->iq_i, -cur->v_lim, cur->v_lim);
  Foc_LimitDQ(cur->v_lim, &cur->vd, &cur->vq);
}
