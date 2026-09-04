#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"
#include "motor_control_config.h"

#include <stdbool.h>

typedef enum
{
  MOTOR_CONTROL_PHASE_U = 0,
  MOTOR_CONTROL_PHASE_V,
  MOTOR_CONTROL_PHASE_W
} MotorControlPhase;

/**
 * @brief Start three-phase complementary PWM at the 50% midpoint duty.
 * @param htim Advanced-control timer with CH1/CH1N through CH3/CH3N.
 */
HAL_StatusTypeDef MotorControl_Init(TIM_HandleTypeDef *htim);

/**
 * @brief Parse and apply one serial command.
 *
 * Supported commands:
 *   <offset>    Apply an offset in percent to the currently selected phase.
 *   u <offset>  Select U and apply the offset. V and W return to 50%.
 *   v <offset>  Select V and apply the offset. U and W return to 50%.
 *   w <offset>  Select W and apply the offset. U and V return to 50%.
 *   mid         Return all phases to 50%.
 *   stop        Disable all TIM1 PWM outputs.
 *   start       Restart all phases at 50%.
 *   run cw <rpm>    Start clockwise open-loop six-step drive.
 *   run ccw <rpm>   Start counter-clockwise open-loop six-step drive.
 *   status      Print the current state.
 *
 * @return true when the command was valid, otherwise false.
 */
bool MotorControl_ProcessCommand(const char *command);

/**
 * @brief Process only the emergency "stop" command.
 * @return true when a stop command was handled.
 */
bool MotorControl_ProcessStopCommand(const char *command);

/** @brief Immediately disable all main and complementary PWM outputs. */
void MotorControl_Stop(void);

/**
 * @brief Return the active six-step sector (1 through 6), or 0 otherwise.
 *
 * The value is updated atomically with each commutation and can be sampled
 * from the ADC interrupt.
 */
uint8_t MotorControl_GetSector(void);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_CONTROL_H */
