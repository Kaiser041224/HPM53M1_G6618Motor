#ifndef SERVO_H
#define SERVO_H

#include <stdint.h>
#include "foc_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  SERVO_IDLE = 0,
  SERVO_OPENLOOP,
  SERVO_VOLTAGE,
  SERVO_RUN,
  SERVO_FAULT
} ServoMode_t;

#define SERVO_CTRL_MOTION           (0x00U)
#define SERVO_CTRL_VELOCITY         (0x01U)
#define SERVO_CTRL_POSITION         (0x02U)

typedef struct
{
  float p_set;
  float v_set;
  float kp;
  float kd;
  float t_ff;
  float t_ref;
  float p_act;
  float v_act;
  float elec;
  float ia;
  float ib;
  float id;
  float iq;
  int8_t closed_loop_dir;
  uint8_t ctrl_mode;
  uint8_t inner_loop;
} ServoTelemetry_t;

void Servo_Init(void);
void Servo_OnPwmIsr(void);
void Servo_OnCtrlIsr(void);

void Servo_SetMode(ServoMode_t mode);
ServoMode_t Servo_GetMode(void);
void Servo_SetOpenloop(float d_v, float q_v, float elec_rate_rad_s, float elec_angle_rad);
void Servo_SetOpenloopRate(float elec_rate_rad_s);
float Servo_GetOpenloopElec(void);
void Servo_SetVoltageCmd(float d_v, float q_v);
void Servo_SetMotion(float p_set, float v_set, float t_ff);
void Servo_SetVelocityCmd(float v_set);
void Servo_SetPositionCmd(float p_set, float v_max);
void Servo_SetGains(float kp, float kd);
void Servo_GetGains(float *kp, float *kd);
void Servo_SetVelocityGains(float kp, float ki);
void Servo_GetVelocityGains(float *kp, float *ki);
void Servo_SetPositionGains(float kp, float ki, float kd);
void Servo_GetPositionGains(float *kp, float *ki, float *kd);
void Servo_SetCtrlMode(uint8_t mode);
uint8_t Servo_GetCtrlMode(void);

/**
 * @brief  选择运控内环：`CFG_INNER_LOOP_VOLTAGE` 或 `CFG_INNER_LOOP_CURRENT`。
 *         开环/电压校准路径始终走电压；切换时复位电流 PI。
 */
void Servo_SetInnerLoop(uint8_t mode);
uint8_t Servo_GetInnerLoop(void);
void Servo_SetZero(void);
uint8_t Servo_SeedMultiTurn(float zero_motor_rad);
float Servo_GetZeroMotor(void);
void Servo_SetClosedLoopDir(int8_t dir);
void Servo_SetEncoderAlignment(int8_t encoder_dir, float electrical_offset_rad);
void Servo_SetPolePairs(uint8_t pole_pairs);
void Servo_HoldPosition(void);
void Servo_GetTelemetry(ServoTelemetry_t *out);
Foc_Encoder_t *Servo_GetEncoder(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_H */
