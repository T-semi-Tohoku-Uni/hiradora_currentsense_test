#include "console.h"

#include <stdint.h>

#if (CONSOLE_TX_BUFFER_SIZE < 2U) || (CONSOLE_TX_BUFFER_SIZE > 65535U)
#error "CONSOLE_TX_BUFFER_SIZE must be between 2 and 65535"
#endif

/* Console_Init()で登録される、コンソール出力用のUARTです。 */
static UART_HandleTypeDef *console_uart;

/*
 * printfから受け取った文字列を一時保存するリングバッファです。
 *
 * tx_write_index : 次のデータを書き込む位置
 * tx_read_index  : 次のDMA送信を開始する位置
 *
 * 各インデックスはバッファ末尾まで進むと0へ戻ります。
 * 空と満杯を区別するため、常に1バイト分を未使用にします。
 */
static uint8_t tx_buffer[CONSOLE_TX_BUFFER_SIZE];
static volatile uint16_t tx_write_index;
static volatile uint16_t tx_read_index;

/*
 * 以下の変数はmain側とDMA完了割り込み側の両方から参照します。
 * volatileを付け、コンパイラによる不要な読み書きの省略を防ぎます。
 */
static volatile uint16_t current_dma_length;
static volatile uint8_t is_dma_transmitting;

/**
 * @brief バッファに未送信データがあればDMA送信を開始します。
 *
 * すでにDMA送信中の場合は何もせずに戻ります。
 * リングバッファの末尾をまたぐデータは、前半と後半の2回に分けて
 * DMA送信します。
 */
static void Console_StartTransmit(void)
{
  uint16_t transmit_length;
  uint32_t interrupt_state;

  /*
   * DMA完了割り込みと同時に変数を書き換えないよう、一時的に
   * 割り込みを禁止します。元の割り込み状態は後で復元します。
   */
  interrupt_state = __get_PRIMASK();
  __disable_irq();

  if ((console_uart == NULL) ||
      (is_dma_transmitting != 0U) ||
      (tx_write_index == tx_read_index))
  {
    if (interrupt_state == 0U)
    {
      __enable_irq();
    }
    return;
  }

  /* 現在の読み出し位置から連続して並んでいるデータ数を求めます。 */
  if (tx_write_index > tx_read_index)
  {
    transmit_length = (uint16_t)(tx_write_index - tx_read_index);
  }
  else
  {
    /* 書き込み位置が一周している場合は、まずバッファ末尾まで送ります。 */
    transmit_length = (uint16_t)(CONSOLE_TX_BUFFER_SIZE - tx_read_index);
  }

  current_dma_length = transmit_length;
  is_dma_transmitting = 1U;

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }

  /* DMAへ、送信元アドレスと送信バイト数を渡します。 */
  if (HAL_UART_Transmit_DMA(console_uart,
                            &tx_buffer[tx_read_index],
                            transmit_length) != HAL_OK)
  {
    /* 開始できなかった場合、次回再試行できるよう送信中状態を解除します。 */
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    is_dma_transmitting = 0U;
    current_dma_length = 0U;
    if (interrupt_state == 0U)
    {
      __enable_irq();
    }
  }
}

void Console_Init(UART_HandleTypeDef *huart)
{
  uint32_t interrupt_state;

  /* 初期化の途中でDMA完了割り込みが入らないようにします。 */
  interrupt_state = __get_PRIMASK();
  __disable_irq();

  console_uart = huart;
  tx_write_index = 0U;
  tx_read_index = 0U;
  current_dma_length = 0U;
  is_dma_transmitting = 0U;

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }
}

size_t Console_Write(const void *data, size_t length)
{
  const uint8_t *source_data = (const uint8_t *)data;
  size_t written_length = 0U;

  /* NULLポインタや、Console_Init前の呼び出しでは送信しません。 */
  if ((source_data == NULL) || (console_uart == NULL))
  {
    return 0U;
  }

  /* 受け取ったデータを1バイトずつリングバッファへ追加します。 */
  while (written_length < length)
  {
    uint16_t next_write_index;
    uint32_t interrupt_state;

    /* DMA完了割り込み側もインデックスを操作するため、ここでは排他します。 */
    interrupt_state = __get_PRIMASK();
    __disable_irq();

    next_write_index = (uint16_t)(tx_write_index + 1U);
    if (next_write_index >= CONSOLE_TX_BUFFER_SIZE)
    {
      next_write_index = 0U;
    }

    /* 次の書き込み位置が読み出し位置と同じなら、バッファは満杯です。 */
    if (next_write_index != tx_read_index)
    {
      tx_buffer[tx_write_index] = source_data[written_length];
      tx_write_index = next_write_index;
      written_length++;
    }

    if (interrupt_state == 0U)
    {
      __enable_irq();
    }

    /* DMAが停止中なら、ここで送信を開始します。 */
    Console_StartTransmit();

    /*
     * 割り込み処理内では、バッファの空きを待ち続けることができません。
     * 満杯の場合は、ここまでに追加できたバイト数を返して終了します。
     */
    if ((next_write_index == tx_read_index) && (__get_IPSR() != 0U))
    {
      break;
    }
  }

  return written_length;
}

HAL_StatusTypeDef Console_Flush(uint32_t timeout_ms)
{
  uint32_t start_tick = HAL_GetTick();

  if (console_uart == NULL)
  {
    return HAL_ERROR;
  }

  /* バッファが空になり、DMA送信も終了するまで待ちます。 */
  while ((tx_write_index != tx_read_index) || (is_dma_transmitting != 0U))
  {
    /* 何らかの理由でDMAが始まっていなければ、ここで再試行します。 */
    Console_StartTransmit();

    if ((HAL_GetTick() - start_tick) >= timeout_ms)
    {
      return HAL_TIMEOUT;
    }
  }

  return HAL_OK;
}

int _write(int file, char *data, int length)
{
  size_t written_length;

  /* fileはstdout/stderrの識別子ですが、この実装では同じUARTへ送ります。 */
  (void)file;

  if ((data == NULL) || (length <= 0))
  {
    return 0;
  }

  /* printfは最終的に_writeを呼ぶため、ここからDMA送信へ接続します。 */
  written_length = Console_Write(data, (size_t)length);
  return (int)written_length;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  uint32_t interrupt_state;

  /* コンソール以外のUARTで発生した完了通知は処理しません。 */
  if ((huart != console_uart) || (is_dma_transmitting == 0U))
  {
    return;
  }

  /*
   * この関数は、UARTのDMA送信が完了したときにHALから呼ばれます。
   * 送信済みのバイト数だけ読み出し位置を進めます。
   */
  interrupt_state = __get_PRIMASK();
  __disable_irq();

  tx_read_index = (uint16_t)(tx_read_index + current_dma_length);
  if (tx_read_index >= CONSOLE_TX_BUFFER_SIZE)
  {
    tx_read_index = (uint16_t)(tx_read_index - CONSOLE_TX_BUFFER_SIZE);
  }

  current_dma_length = 0U;
  is_dma_transmitting = 0U;

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }

  /* バッファに続きがあれば、次のDMA送信を開始します。 */
  Console_StartTransmit();
}
