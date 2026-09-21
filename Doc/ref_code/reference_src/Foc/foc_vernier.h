#ifndef FOC_VERNIER_H
#define FOC_VERNIER_H

#include <stdint.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FOC_VERNIER_MOTOR_TURNS   ((int32_t)CFG_VERNIER_TEETH2)

/**
 * @brief  Recover the motor-shaft absolute angle from a 30/31 vernier pair.
 * @param[in]  raw1         Motor-end single-turn code.
 * @param[in]  raw2         Aux-gear single-turn code.
 * @param[out] raw_abs_out  Motor absolute angle in the sensor frame, [0, 2π N2).
 * @return 1 if the residue is consistent with an integer turn.
 */
uint8_t Foc_VernierRawAbs(uint16_t raw1, uint16_t raw2, float *raw_abs_out);

/**
 * @brief  Map a sensor-frame absolute angle into the FOC encoder-dir frame.
 * @param[in] raw_abs      Sensor-frame motor absolute angle.
 * @param[in] encoder_dir  Motor encoder direction from calibration (+1 / -1).
 * @return Aligned multi-turn mechanical angle.
 */
float Foc_VernierAlignAbs(float raw_abs, int8_t encoder_dir);

#ifdef __cplusplus
}
#endif

#endif /* FOC_VERNIER_H */
