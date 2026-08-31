#include "current_sense.h"

#include "console.h"

#include <ctype.h>
#include <stdio.h>

#define CURRENT_SENSE_PWM_OUTPUT_MASK                                  \
  (TIM_CCER_CC1E | TIM_CCER_CC1NE | TIM_CCER_CC2E | TIM_CCER_CC2NE | \
   TIM_CCER_CC3E | TIM_CCER_CC3NE)

#define CURRENT_SENSE_CONSOLE_FLUSH_TIMEOUT_MS 15000U
#define CURRENT_SENSE_ACQUISITION_TIMEOUT_MS 1000U

typedef struct
{
  uint16_t u_raw;
  uint16_t v_raw;
} CurrentSenseSample;

typedef enum
{
  CURRENT_SENSE_UNINITIALIZED = 0,
  CURRENT_SENSE_IDLE,
  CURRENT_SENSE_ACQUIRING,
  CURRENT_SENSE_DATA_READY,
  CURRENT_SENSE_TRANSMITTING
} CurrentSenseState;

static ADC_HandleTypeDef *adc_master;
static ADC_HandleTypeDef *adc_slave;
static TIM_HandleTypeDef *sample_timer;
static CurrentSenseSample samples[CURRENT_SENSE_SAMPLE_COUNT];
static volatile uint32_t captured_sample_count;
static volatile CurrentSenseState current_state = CURRENT_SENSE_UNINITIALIZED;
static uint32_t acquisition_start_tick;
static bool timer_started_for_capture;

static void CurrentSense_DisableTrigger(void)
{
  /* Disable only CH4 so running motor PWM channels are unaffected. */
  CLEAR_BIT(sample_timer->Instance->CCER, TIM_CCER_CC4E);

  if (timer_started_for_capture)
  {
    CLEAR_BIT(sample_timer->Instance->CR1, TIM_CR1_CEN);
    CLEAR_BIT(sample_timer->Instance->BDTR, TIM_BDTR_MOE);
    __HAL_TIM_SET_COUNTER(sample_timer, 0U);
    timer_started_for_capture = false;
  }
}

static const char *CurrentSense_SkipSpaces(const char *text)
{
  while ((*text != '\0') && (isspace((unsigned char)*text) != 0))
  {
    text++;
  }
  return text;
}

static bool CurrentSense_IsCommand(const char *text, const char *expected)
{
  text = CurrentSense_SkipSpaces(text);

  while ((*text != '\0') && (*expected != '\0'))
  {
    if (tolower((unsigned char)*text) != tolower((unsigned char)*expected))
    {
      return false;
    }
    text++;
    expected++;
  }

  text = CurrentSense_SkipSpaces(text);
  return ((*text == '\0') && (*expected == '\0'));
}

static HAL_StatusTypeDef CurrentSense_StartAcquisition(void)
{
  HAL_StatusTypeDef status;

  /* Keep prior console messages outside the CSV response. */
  if (Console_Flush(1000U) != HAL_OK)
  {
    printf("ADC capture could not flush the console\r\n");
    return HAL_ERROR;
  }

  timer_started_for_capture =
    (READ_BIT(sample_timer->Instance->CR1, TIM_CR1_CEN) == 0U);

  /*
   * The ADC trigger uses the internal CH4 output edge, not merely CC4IF.
   * Expose CH4 before arming the ADC so enabling it cannot become sample 0.
   */
  __HAL_TIM_CLEAR_FLAG(sample_timer, TIM_FLAG_CC4);
  SET_BIT(sample_timer->Instance->CCER, TIM_CCER_CC4E);

  if (timer_started_for_capture)
  {
    /* Generate CH4 internally while keeping all motor pins disabled. */
    CLEAR_BIT(sample_timer->Instance->BDTR, TIM_BDTR_MOE);
    CLEAR_BIT(sample_timer->Instance->CCER,
              CURRENT_SENSE_PWM_OUTPUT_MASK);
    __HAL_TIM_SET_COUNTER(sample_timer, 0U);
    sample_timer->Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(sample_timer, TIM_FLAG_UPDATE | TIM_FLAG_CC4);
    SET_BIT(sample_timer->Instance->BDTR, TIM_BDTR_MOE);
    SET_BIT(sample_timer->Instance->CR1, TIM_CR1_CEN);
  }

  captured_sample_count = 0U;
  current_state = CURRENT_SENSE_ACQUIRING;

  /* In dual injected mode, the slave must be armed before the master. */
  status = HAL_ADCEx_InjectedStart_IT(adc_slave);
  if (status != HAL_OK)
  {
    CurrentSense_DisableTrigger();
    current_state = CURRENT_SENSE_IDLE;
    return status;
  }

  status = HAL_ADCEx_InjectedStart_IT(adc_master);
  if (status != HAL_OK)
  {
    (void)HAL_ADCEx_InjectedStop_IT(adc_slave);
    CurrentSense_DisableTrigger();
    current_state = CURRENT_SENSE_IDLE;
    return status;
  }

  acquisition_start_tick = HAL_GetTick();

  return HAL_OK;
}

static HAL_StatusTypeDef CurrentSense_StopAcquisition(void)
{
  HAL_StatusTypeDef result = HAL_OK;

  CurrentSense_DisableTrigger();

  /* Stop the master before the slave as required for dual injected mode. */
  if (HAL_ADCEx_InjectedStop_IT(adc_master) != HAL_OK)
  {
    result = HAL_ERROR;
  }
  if (HAL_ADCEx_InjectedStop_IT(adc_slave) != HAL_OK)
  {
    result = HAL_ERROR;
  }

  return result;
}

static void CurrentSense_SendCsv(void)
{
  static const char csv_header[] = "sample,u_raw,v_raw\r\n";
  char line[32];
  uint32_t index;

  (void)Console_Write(csv_header, sizeof(csv_header) - 1U);

  for (index = 0U; index < CURRENT_SENSE_SAMPLE_COUNT; index++)
  {
    const int length = snprintf(line,
                                sizeof(line),
                                "%lu,%u,%u\r\n",
                                (unsigned long)index,
                                (unsigned int)samples[index].u_raw,
                                (unsigned int)samples[index].v_raw);

    if ((length > 0) && ((size_t)length < sizeof(line)))
    {
      (void)Console_Write(line, (size_t)length);
    }
  }

  (void)Console_Flush(CURRENT_SENSE_CONSOLE_FLUSH_TIMEOUT_MS);
}

HAL_StatusTypeDef CurrentSense_Init(ADC_HandleTypeDef *master_adc,
                                    ADC_HandleTypeDef *slave_adc,
                                    OPAMP_HandleTypeDef *u_opamp,
                                    OPAMP_HandleTypeDef *v_opamp,
                                    TIM_HandleTypeDef *trigger_timer)
{
  if ((master_adc == NULL) || (slave_adc == NULL) ||
      (u_opamp == NULL) || (v_opamp == NULL) ||
      (trigger_timer == NULL) ||
      (master_adc->Instance != ADC1) || (slave_adc->Instance != ADC2) ||
      (trigger_timer->Instance != TIM1))
  {
    return HAL_ERROR;
  }

  adc_master = master_adc;
  adc_slave = slave_adc;
  sample_timer = trigger_timer;

  if (HAL_ADCEx_Calibration_Start(adc_master, ADC_SINGLE_ENDED) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_ADCEx_Calibration_Start(adc_slave, ADC_SINGLE_ENDED) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_OPAMP_Start(u_opamp) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_OPAMP_Start(v_opamp) != HAL_OK)
  {
    (void)HAL_OPAMP_Stop(u_opamp);
    return HAL_ERROR;
  }

  captured_sample_count = 0U;
  timer_started_for_capture = false;
  current_state = CURRENT_SENSE_IDLE;
  return HAL_OK;
}

bool CurrentSense_ProcessCommand(const char *command)
{
  HAL_StatusTypeDef status;

  if ((command == NULL) || !CurrentSense_IsCommand(command, "adc"))
  {
    return false;
  }

  if (current_state == CURRENT_SENSE_UNINITIALIZED)
  {
    printf("ADC capture is not initialized\r\n");
    return true;
  }
  if (current_state != CURRENT_SENSE_IDLE)
  {
    printf("ADC capture is busy\r\n");
    return true;
  }

  status = CurrentSense_StartAcquisition();
  if (status != HAL_OK)
  {
    printf("ADC capture start failed: HAL status=%d\r\n", (int)status);
  }
  else
  {
    printf("ADC capture started: %u samples\r\n",
           (unsigned int)CURRENT_SENSE_SAMPLE_COUNT);
  }

  return true;
}

void CurrentSense_Task(void)
{
  if (current_state == CURRENT_SENSE_ACQUIRING)
  {
    if ((HAL_GetTick() - acquisition_start_tick) <
        CURRENT_SENSE_ACQUISITION_TIMEOUT_MS)
    {
      return;
    }

    const uint32_t timer_count = sample_timer->Instance->CNT;
    const uint32_t adc1_isr = adc_master->Instance->ISR;
    const uint32_t adc1_cr = adc_master->Instance->CR;
    const uint32_t adc1_ier = adc_master->Instance->IER;
    const uint32_t adc2_isr = adc_slave->Instance->ISR;
    const uint32_t adc2_cr = adc_slave->Instance->CR;

    current_state = CURRENT_SENSE_TRANSMITTING;
    (void)CurrentSense_StopAcquisition();
    printf("ADC capture timed out: samples=%lu, TIM1_CNT=%lu, "
           "ADC1_ISR=0x%08lX, ADC1_CR=0x%08lX, ADC1_IER=0x%08lX, "
           "ADC2_ISR=0x%08lX, ADC2_CR=0x%08lX\r\n",
           (unsigned long)captured_sample_count,
           (unsigned long)timer_count,
           (unsigned long)adc1_isr,
           (unsigned long)adc1_cr,
           (unsigned long)adc1_ier,
           (unsigned long)adc2_isr,
           (unsigned long)adc2_cr);
    current_state = CURRENT_SENSE_IDLE;
    return;
  }

  if (current_state != CURRENT_SENSE_DATA_READY)
  {
    return;
  }

  current_state = CURRENT_SENSE_TRANSMITTING;

  if (CurrentSense_StopAcquisition() != HAL_OK)
  {
    printf("ADC capture stop failed\r\n");
    current_state = CURRENT_SENSE_IDLE;
    return;
  }

  CurrentSense_SendCsv();
  current_state = CURRENT_SENSE_IDLE;
}

bool CurrentSense_IsBusy(void)
{
  return ((current_state == CURRENT_SENSE_ACQUIRING) ||
          (current_state == CURRENT_SENSE_DATA_READY) ||
          (current_state == CURRENT_SENSE_TRANSMITTING));
}

void HAL_ADCEx_InjectedConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  uint32_t index;

  if ((hadc != adc_master) || (current_state != CURRENT_SENSE_ACQUIRING))
  {
    return;
  }

  index = captured_sample_count;
  if (index >= CURRENT_SENSE_SAMPLE_COUNT)
  {
    return;
  }

  samples[index].u_raw =
    (uint16_t)HAL_ADCEx_InjectedGetValue(adc_master, ADC_INJECTED_RANK_1);
  samples[index].v_raw =
    (uint16_t)HAL_ADCEx_InjectedGetValue(adc_slave, ADC_INJECTED_RANK_1);
  __HAL_ADC_CLEAR_FLAG(adc_slave, ADC_FLAG_JEOC | ADC_FLAG_JEOS);

  index++;
  captured_sample_count = index;

  if (index >= CURRENT_SENSE_SAMPLE_COUNT)
  {
    /* Main context performs the blocking HAL stop calls and CSV output. */
    __HAL_ADC_DISABLE_IT(adc_master, ADC_IT_JEOC | ADC_IT_JEOS);
    __HAL_ADC_DISABLE_IT(adc_slave, ADC_IT_JEOC | ADC_IT_JEOS);
    current_state = CURRENT_SENSE_DATA_READY;
  }
}
