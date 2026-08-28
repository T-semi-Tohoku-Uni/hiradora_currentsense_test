#ifndef CONSOLE_H
#define CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

#include <stddef.h>
#include <stdint.h>

#ifndef CONSOLE_TX_BUFFER_SIZE
/* printfの文字列を一時的に保存する送信リングバッファのサイズ */
#define CONSOLE_TX_BUFFER_SIZE 512U
#endif

/**
 * @brief コンソール出力に使用するUARTを登録します。
 * @param huart CubeMXが生成したUARTハンドルのアドレス
 *
 * UARTとDMAの初期化が完了した後に、一度だけ呼び出してください。
 * 例: Console_Init(&huart1);
 */
void Console_Init(UART_HandleTypeDef *huart);

/**
 * @brief 指定されたデータをUARTのDMA送信待ちバッファへ追加します。
 * @param data 送信するデータの先頭アドレス
 * @param length 送信するバイト数
 * @return バッファへ追加できたバイト数
 *
 * main関数から呼んだ場合、バッファが満杯なら空きができるまで待ちます。
 * 割り込み処理から呼んだ場合は、満杯になった時点で処理を終了します。
 */
size_t Console_Write(const void *data, size_t length);

/**
 * @brief バッファ内の全データが送信されるまで待ちます。
 * @param timeout_ms 最大待ち時間（ミリ秒）
 * @return 送信完了ならHAL_OK、時間切れならHAL_TIMEOUT
 */
HAL_StatusTypeDef Console_Flush(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_H */
