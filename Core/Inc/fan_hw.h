/**
  ******************************************************************************
  * @file    fan_hw.h
  * @brief   2-channel fan interface on STM32F103C8T6 (Blue Pill).
  *
  *   Serial (CDC) protocol: host sends "D<ch>:<duty>" lines, firmware replies
  *   "R<ch>:<rpm>" lines every second. Two physical channels.
  *
  *   PWM (25 kHz, open-drain AF + internal pull-up; F1 default AF, no remap):
  *     ch0: TIM4_CH2 / PB7
  *     ch1: TIM4_CH3 / PB8
  *
  *   Tach (EXTI falling edge, internal pull-up, 2 pulses/rev):
  *     ch0: PA1   (EXTI1)
  *     ch1: PB14  (EXTI14)
  *
  *   Status LED: PC13 (Blue Pill onboard, active-low).
  ******************************************************************************
  */

#ifndef __FAN_HW_H
#define __FAN_HW_H

#include <stdint.h>

#define FAN_HW_CHANNELS 2U

void FanHw_Init(void);
void FanHw_SetDuty(uint8_t ch, uint8_t duty);
uint8_t FanHw_GetDuty(uint8_t ch);
uint16_t FanHw_GetRpm(uint8_t ch);
void FanHw_Tick(uint32_t ms);
void FanHw_UpdateLeds(void);
void FanHw_TachEdge(uint8_t ch);

#endif /* __FAN_HW_H */
