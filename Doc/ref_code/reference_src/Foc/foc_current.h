#ifndef FOC_CURRENT_H
#define FOC_CURRENT_H

#include <stdint.h>
#include "foc_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  float ia;
  float ib;
  float id;
  float iq;
  float vd;
  float vq;
  float id_i;
  float iq_i;
  float kp;
  float ki;
  float kp_v;
  float ki_v;
  float i_lim;
  float v_lim;
} Foc_Current_t;

void Foc_CurrentInit(Foc_Current_t *cur, float r_ohm, float l_h, float bw_hz,
                     float vbus_v, float i_lim, float v_lim);

/**
 * @brief  Scale pu gains as Kp=Lωc/Vbus, Ki=Rωc/Vbus. Invalid Vbus uses CFG_VBUS_NOMINAL_V.
 */
void Foc_CurrentSetVbus(Foc_Current_t *cur, float vbus_v);
void Foc_CurrentReset(Foc_Current_t *cur);

/**
 * @brief  Clarke/Park; optionally close Id/Iq PI.
 * @param  run_pi  0: observe only and clear integrators. 1: PI → vd/vq.
 */
void Foc_CurrentStep(Foc_Current_t *cur, float ia, float ib, float elec_rad,
                     float id_ref, float iq_ref, float dt_s, uint8_t run_pi);

#ifdef __cplusplus
}
#endif

#endif /* FOC_CURRENT_H */
