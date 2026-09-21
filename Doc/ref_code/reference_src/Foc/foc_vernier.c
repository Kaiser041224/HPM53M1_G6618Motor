#include "foc_vernier.h"

#include "encoder.h"
#include "foc_math.h"

#include <math.h>

#define FOC_VERNIER_RESIDUE_TOL   (0.35f)

static float Foc_VernierWrap01(float x)
{
  x -= floorf(x);
  if (x < 0.0f)
  {
    x += 1.0f;
  }
  else if (x >= 1.0f)
  {
    x -= 1.0f;
  }
  return x;
}

uint8_t Foc_VernierRawAbs(uint16_t raw1, uint16_t raw2, float *raw_abs_out)
{
  const float inv_cpr = 1.0f / (float)ENCODER_CPR;
  const float n1 = (float)CFG_VERNIER_TEETH1;
  const float n2 = (float)CFG_VERNIER_TEETH2;
  float u;
  float v;
  float residual;
  float n_f;
  int32_t n;

  if (raw_abs_out == 0)
  {
    return 0U;
  }

  u = (float)raw1 * inv_cpr;
  v = (float)CFG_ENC2_DIR * ((float)raw2 * inv_cpr);
  v = Foc_VernierWrap01(v - (CFG_ENC2_OFFSET_RAD * (1.0f / FOC_TWO_PI)));

  /* Meshed gears: v + (N1/N2)*u ≡ n/N2 (mod 1), n = 0..N2-1. */
  residual = Foc_VernierWrap01(v + ((n1 / n2) * u));
  n_f = residual * n2;
  n = (int32_t)(n_f + 0.5f);
  if (n >= (int32_t)CFG_VERNIER_TEETH2)
  {
    n = 0;
  }
  if (n < 0)
  {
    n += (int32_t)CFG_VERNIER_TEETH2;
  }
  {
    float n_cmp = (float)n;
    if ((n == 0) && (n_f > (n2 * 0.5f)))
    {
      n_cmp = n2;
    }
    if (fabsf(n_f - n_cmp) > FOC_VERNIER_RESIDUE_TOL)
    {
      return 0U;
    }
  }

  *raw_abs_out = (((float)n + u) * FOC_TWO_PI);
  return 1U;
}

float Foc_VernierAlignAbs(float raw_abs, int8_t encoder_dir)
{
  if (encoder_dir >= 0)
  {
    return raw_abs;
  }
  return -raw_abs;
}
