#include "motor_control.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MOTOR_CONTROL_OUTPUT_ENABLE_MASK                                  \
  (TIM_CCER_CC1E | TIM_CCER_CC1NE | TIM_CCER_CC2E | TIM_CCER_CC2NE |    \
   TIM_CCER_CC3E | TIM_CCER_CC3NE)

static TIM_HandleTypeDef *motor_timer;
static MotorControlPhase selected_phase = MOTOR_CONTROL_PHASE_U;
static float selected_offset_percent;
static bool outputs_enabled;

static const char *MotorControl_SkipSpaces(const char *text)
{
  while ((*text != '\0') && (isspace((unsigned char)*text) != 0))
  {
    text++;
  }
  return text;
}

static bool MotorControl_IsCommand(const char *text, const char *expected)
{
  while ((*text != '\0') && (*expected != '\0'))
  {
    if (tolower((unsigned char)*text) != tolower((unsigned char)*expected))
    {
      return false;
    }
    text++;
    expected++;
  }

  text = MotorControl_SkipSpaces(text);
  return ((*text == '\0') && (*expected == '\0'));
}

static const char *MotorControl_PhaseName(MotorControlPhase phase)
{
  static const char *const phase_names[] = {"U", "V", "W"};
  return phase_names[(unsigned int)phase];
}

static uint32_t MotorControl_MidpointCompare(void)
{
  return (__HAL_TIM_GET_AUTORELOAD(motor_timer) + 1U) / 2U;
}

static void MotorControl_WriteMidpoint(void)
{
  const uint32_t midpoint = MotorControl_MidpointCompare();

  __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_1, midpoint);
  __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_2, midpoint);
  __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_3, midpoint);
}

static void MotorControl_StartAtMidpoint(void)
{
  TIM_TypeDef *tim = motor_timer->Instance;
  uint32_t interrupt_state = __get_PRIMASK();

  __disable_irq();

  /* Keep every half bridge disabled until all preload values are ready. */
  CLEAR_BIT(tim->BDTR, TIM_BDTR_MOE);
  CLEAR_BIT(tim->CR1, TIM_CR1_CEN);
  CLEAR_BIT(tim->CCER, MOTOR_CONTROL_OUTPUT_ENABLE_MASK);

  MotorControl_WriteMidpoint();
  __HAL_TIM_SET_COUNTER(motor_timer, 0U);
  tim->EGR = TIM_EGR_UG;
  __HAL_TIM_CLEAR_FLAG(motor_timer, TIM_FLAG_UPDATE);

  /* Enable all six pins together, then start the counter and main output. */
  SET_BIT(tim->CCER, MOTOR_CONTROL_OUTPUT_ENABLE_MASK);
  SET_BIT(tim->CR1, TIM_CR1_CEN);
  SET_BIT(tim->BDTR, TIM_BDTR_MOE);

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }

  selected_offset_percent = 0.0f;
  outputs_enabled = true;
}

static bool MotorControl_ParseOffset(const char *text, float *offset_percent)
{
  char *parse_end;
  float parsed_value;

  text = MotorControl_SkipSpaces(text);
  errno = 0;
  parsed_value = strtof(text, &parse_end);

  if ((parse_end == text) || (errno == ERANGE) || !isfinite(parsed_value))
  {
    return false;
  }

  parse_end = (char *)MotorControl_SkipSpaces(parse_end);
  if (*parse_end != '\0')
  {
    return false;
  }

  if ((parsed_value < -MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT) ||
      (parsed_value > MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT))
  {
    printf("Offset must be between %.1f and +%.1f %%\r\n",
           -MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT,
           MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT);
    return false;
  }

  *offset_percent = parsed_value;
  return true;
}

static void MotorControl_ApplyOffset(MotorControlPhase phase, float offset_percent)
{
  const float timer_counts =
    (float)(__HAL_TIM_GET_AUTORELOAD(motor_timer) + 1U);
  const float compare_value =
    timer_counts * (0.5f + (offset_percent / 100.0f));
  const uint32_t rounded_compare =
    (uint32_t)(compare_value + ((compare_value >= 0.0f) ? 0.5f : -0.5f));

  /* CCR preload makes all three new values effective at the same update event. */
  MotorControl_WriteMidpoint();

  switch (phase)
  {
    case MOTOR_CONTROL_PHASE_U:
      __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_1, rounded_compare);
      break;

    case MOTOR_CONTROL_PHASE_V:
      __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_2, rounded_compare);
      break;

    case MOTOR_CONTROL_PHASE_W:
      __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_3, rounded_compare);
      break;

    default:
      return;
  }

  selected_phase = phase;
  selected_offset_percent = offset_percent;

  printf("PWM: %s=%.2f %%, others=50.00 %% (offset %+.2f %%)\r\n",
         MotorControl_PhaseName(phase),
         50.0f + offset_percent,
         offset_percent);
}

HAL_StatusTypeDef MotorControl_Init(TIM_HandleTypeDef *htim)
{
  if ((htim == NULL) ||
      !IS_TIM_CCXN_INSTANCE(htim->Instance, TIM_CHANNEL_3) ||
      (MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT <= 0.0f) ||
      (MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT >= 50.0f))
  {
    return HAL_ERROR;
  }

  motor_timer = htim;
  selected_phase = MOTOR_CONTROL_PHASE_U;
  MotorControl_StartAtMidpoint();
  return HAL_OK;
}

bool MotorControl_ProcessCommand(const char *command)
{
  const char *argument;
  MotorControlPhase command_phase = selected_phase;
  float offset_percent;

  if ((motor_timer == NULL) || (command == NULL))
  {
    return false;
  }

  command = MotorControl_SkipSpaces(command);

  if (MotorControl_IsCommand(command, "stop"))
  {
    MotorControl_Stop();
    printf("PWM stopped\r\n");
    return true;
  }

  if (MotorControl_IsCommand(command, "start"))
  {
    MotorControl_StartAtMidpoint();
    printf("PWM started: U=V=W=50.00 %%\r\n");
    return true;
  }

  if (MotorControl_IsCommand(command, "mid"))
  {
    if (!outputs_enabled)
    {
      printf("PWM is stopped; send 'start' first\r\n");
      return false;
    }
    MotorControl_WriteMidpoint();
    selected_offset_percent = 0.0f;
    printf("PWM: U=V=W=50.00 %%\r\n");
    return true;
  }

  if (MotorControl_IsCommand(command, "status"))
  {
    printf("PWM: %s, selected=%s, offset=%+.2f %%\r\n",
           outputs_enabled ? "running" : "stopped",
           MotorControl_PhaseName(selected_phase),
           selected_offset_percent);
    return true;
  }

  argument = command;
  if (((command[0] == 'u') || (command[0] == 'U') ||
       (command[0] == 'v') || (command[0] == 'V') ||
       (command[0] == 'w') || (command[0] == 'W')) &&
      (isspace((unsigned char)command[1]) != 0))
  {
    switch (tolower((unsigned char)command[0]))
    {
      case 'u': command_phase = MOTOR_CONTROL_PHASE_U; break;
      case 'v': command_phase = MOTOR_CONTROL_PHASE_V; break;
      case 'w': command_phase = MOTOR_CONTROL_PHASE_W; break;
      default: break;
    }
    argument = &command[1];
  }

  if (!MotorControl_ParseOffset(argument, &offset_percent))
  {
    printf("Invalid command. Use: <offset>, u/v/w <offset>, mid, stop, start, status\r\n");
    return false;
  }

  if (!outputs_enabled)
  {
    printf("PWM is stopped; send 'start' first\r\n");
    return false;
  }

  MotorControl_ApplyOffset(command_phase, offset_percent);
  return true;
}

void MotorControl_Stop(void)
{
  uint32_t interrupt_state;

  if (motor_timer == NULL)
  {
    return;
  }

  interrupt_state = __get_PRIMASK();
  __disable_irq();

  CLEAR_BIT(motor_timer->Instance->BDTR, TIM_BDTR_MOE);
  CLEAR_BIT(motor_timer->Instance->CR1, TIM_CR1_CEN);
  CLEAR_BIT(motor_timer->Instance->CCER, MOTOR_CONTROL_OUTPUT_ENABLE_MASK);

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }

  outputs_enabled = false;
}
