/**
  ******************************************************************************
  * @file    fan_hw.c
  * @brief   2-channel fan interface on STM32F103C8T6 (Blue Pill).
  *
  *   PWM on PB7 (TIM4_CH2) and PB8 (TIM4_CH3), push-pull AF. F1 has no
  *   per-pin AF mux: TIM4 default mapping lands on these pins with no AFIO
  *   remap, so the GPIO is just set to AF_PP.
  *
  *   RPM is measured by counting tach falling edges over the tick window.
  *   2 pulses per revolution -> rpm = pulses * 30000 / ms.
  ******************************************************************************
  */

#include "fan_hw.h"
#include "main.h"
#include "stm32f1xx_hal.h"

/* F1 timer clock: SYSCLK 72 MHz, APB1 prescaler /2 -> 36 MHz, x2 -> 72 MHz.
   72e6 / ((PSC+1)*(ARR+1)) = 25000 -> (PSC+1)*(ARR+1) = 2880.
   PSC=1 -> ARR+1=1440 -> ARR=1439 (counter ticks at 36 MHz). */
#define FAN_HW_PWM_PERIOD   1439U
#define FAN_HW_PWM_PSC      1U
#define FAN_HW_DEBOUNCE_MS  2U
#define FAN_HW_START_DUTY   50U   /* safe boot duty: fans run at 50% until told otherwise */

#define FAN_HW_TACH_PIN_0   GPIO_PIN_1     /* PA1  -> EXTI1  */
#define FAN_HW_TACH_PIN_1   GPIO_PIN_14    /* PB14 -> EXTI14 */

static TIM_HandleTypeDef htim4;
static uint8_t fan_duty[FAN_HW_CHANNELS];
static volatile uint32_t tach_count[FAN_HW_CHANNELS];
static volatile uint32_t last_edge[FAN_HW_CHANNELS];
static volatile uint8_t tach_burst[FAN_HW_CHANNELS];
static uint16_t fan_rpm[FAN_HW_CHANNELS];

static void FanHw_SetChannelPwm(uint8_t ch, uint16_t ccr)
{
  switch (ch)
  {
    case 0U:
      __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_2, ccr);
      break;
    case 1U:
      __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_3, ccr);
      break;
    default:
      break;  /* ch2/ch3: no physical output */
  }
}

static void FanHw_StartPwm(void)
{
  HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_2);
  HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_3);
}

void FanHw_Init(void)
{
  uint8_t ch;
  GPIO_InitTypeDef gpio;
  TIM_OC_InitTypeDef oc;

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_AFIO_CLK_ENABLE();
  __HAL_RCC_TIM4_CLK_ENABLE();

  /* Re-enable the SWD debug port (HAL_MspInit disables it when .ioc SYS=No_Debug). */
  __HAL_AFIO_REMAP_SWJ_ENABLE();

  /* PWM: PB7 = TIM4_CH2, PB8 = TIM4_CH3 (F1 default AF, push-pull for a clean
     driven square; open-drain would rely on the internal pull-up). */
  gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &gpio);

  /* Tach: PA1 (EXTI1, ch0) and PB14 (EXTI14, ch1), falling edge, pull-up. */
  gpio.Pin = FAN_HW_TACH_PIN_0;
  gpio.Mode = GPIO_MODE_IT_FALLING;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin = FAN_HW_TACH_PIN_1;
  HAL_GPIO_Init(GPIOB, &gpio);

  HAL_NVIC_SetPriority(EXTI1_IRQn, 6U, 0U);
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 6U, 0U);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* PC13 is a backup-domain pin. The CubeMX RTC init (RTC_OUTPUTSOURCE_ALARM)
     programs it as the RTC "alarm/second" output, which survives system reset
     and overrides the GPIO. Clear that output config so the GPIO LED works. */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_RCC_BKP_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
  BKP->RTCCR &= ~(BKP_RTCCR_CCO | BKP_RTCCR_ASOE | BKP_RTCCR_ASOS);

  /* PC13 onboard LED (Blue Pill: active-low, backup-domain pin -> low speed). */
  gpio.Pin = GPIO_PIN_13;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &gpio);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);  /* LED off */

  /* TIM4: 25 kHz PWM on CH2 (PB7) and CH3 (PB8). */
  htim4.Instance = TIM4;
  htim4.Init.Prescaler = FAN_HW_PWM_PSC;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = FAN_HW_PWM_PERIOD;
  htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  HAL_TIM_PWM_Init(&htim4);

  oc.OCMode = TIM_OCMODE_PWM1;
  oc.Pulse = 0U;
  oc.OCPolarity = TIM_OCPOLARITY_HIGH;
  oc.OCFastMode = TIM_OCFAST_DISABLE;
  HAL_TIM_PWM_ConfigChannel(&htim4, &oc, TIM_CHANNEL_2);
  HAL_TIM_PWM_ConfigChannel(&htim4, &oc, TIM_CHANNEL_3);

  for (ch = 0U; ch < FAN_HW_CHANNELS; ch++)
  {
    fan_duty[ch] = FAN_HW_START_DUTY;
    tach_count[ch] = 0U;
    last_edge[ch] = 0U;
    tach_burst[ch] = 0U;
    fan_rpm[ch] = 0U;
  }

  FanHw_StartPwm();

  for (ch = 0U; ch < FAN_HW_CHANNELS; ch++)
  {
    FanHw_SetChannelPwm(ch, (uint16_t)(((uint32_t)FAN_HW_START_DUTY * (FAN_HW_PWM_PERIOD + 1U)) / 100U));
  }

  FanHw_UpdateLeds();
}

void FanHw_SetDuty(uint8_t ch, uint8_t duty_val)
{
  uint16_t ccr;

  if (ch < FAN_HW_CHANNELS)
  {
    fan_duty[ch] = (duty_val > 100U) ? 100U : duty_val;
    ccr = (uint16_t)(((uint32_t)fan_duty[ch] * (FAN_HW_PWM_PERIOD + 1U)) / 100U);
    FanHw_SetChannelPwm(ch, ccr);
  }
}

uint8_t FanHw_GetDuty(uint8_t ch)
{
  return (ch < FAN_HW_CHANNELS) ? fan_duty[ch] : 0U;
}

void FanHw_Tick(uint32_t ms)
{
  uint8_t ch;

  if (ms == 0U)
  {
    ms = 1U;
  }

  for (ch = 0U; ch < FAN_HW_CHANNELS; ch++)
  {
    uint32_t count = tach_count[ch];
    tach_count[ch] = 0U;
    /* 2 pulses per revolution: rpm = pulses * 30000 / ms */
    fan_rpm[ch] = (uint16_t)((count * 30000U) / ms);
  }
}

uint16_t FanHw_GetRpm(uint8_t ch)
{
  return (ch < FAN_HW_CHANNELS) ? fan_rpm[ch] : 0U;
}

void FanHw_UpdateLeds(void)
{
  /* Single PC13 LED (active-low): on if any real channel has nonzero duty. */
  uint8_t on = (uint8_t)((fan_duty[0U] > 0U) || (fan_duty[1U] > 0U));

  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

void FanHw_TachEdge(uint8_t ch)
{
  uint32_t now;
  uint32_t dt;

  if (ch >= FAN_HW_CHANNELS)
  {
    return;
  }

  now = HAL_GetTick();
  dt = now - last_edge[ch];
  last_edge[ch] = now;

  if (dt < FAN_HW_DEBOUNCE_MS)
  {
    /* Edges closer than the debounce window: high-frequency burst
       (e.g. 25 kHz PWM crosstalk). Suppress the whole burst instead of
       letting one edge through every 2 ms (which read as ~15000 RPM). */
    tach_burst[ch] = 1U;
    return;
  }

  if (tach_burst[ch] != 0U)
  {
    /* First clean edge after a burst: skip it and re-arm. */
    tach_burst[ch] = 0U;
    return;
  }

  tach_count[ch]++;
}

/**
  * @brief  Map EXTI lines to fan tach channels.
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  switch (GPIO_Pin)
  {
    case FAN_HW_TACH_PIN_0:
      FanHw_TachEdge(0U);
      break;
    case FAN_HW_TACH_PIN_1:
      FanHw_TachEdge(1U);
      break;
    default:
      break;
  }
}
