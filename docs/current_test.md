# 2相静的通電による電流検出評価

24 V電源、許容相電流5 Aを想定。ユーザー指定のduty上限は5%です。
相間抵抗が不明なので、5%で5 A以下になる保証はありません。
実機書き込み・通電・波形確認は未実施です。

## 変更と流用

| ファイル | 内容 |
|---|---|
| `Core/Src/motor_control.c` | 起動時OFF、6通電モード、duty指定、nFAULT監視。既存6ステップのセクタ出力処理を共有し、静的モードでは転流しない |
| `Core/Inc/motor_control.h` | コマンドと取得用メタデータAPI |
| `Core/Inc/motor_control_config.h` | 上限5%、ADC取得中の5 A判定 |
| `Core/Src/current_sense.c` | raw/current併記、取得ごとのmode/duty保存、取得中断・ADC異常時停止、取得中の電流超過判定 |
| `Core/Src/main.c` | 起動メッセージ、コマンド案内、Error_Handlerで停止 |
| `Core/Src/stm32g4xx_it.c` | CPU例外時にPWM停止 |
| `CMakeLists.txt` | 既存作業状態でリンク対象から欠落していたHALドライバを重複せず補完 |
| `docs/serial_commands.md`、本書 | CSV仕様・操作・評価手順 |

既存のbootstrap充電、STSPIN32G4のSTATUS/CLEAR、PWM停止、TIM1のセクタ設定、
ADC1/ADC2 injected同時変換、CH4トリガ、raw圧縮保存、UART DMA送信を流用しています。
`.ioc`と編集済みの`cmake/stm32cubemx/CMakeLists.txt`は今回変更していません。
FOC、Clarke/Park、dq、電流PI、エンコーダ制御、新しい自動校正は追加していません。

## PWMと電流経路

TIM1は160 MHz、PSC=0、ARR=3999、center-aligned PWM1（約20 kHz）。
CH1/1N=U、CH2/2N=V、CH3/3N=Wで、全出力はactive highです。
dead-time=60（このクロック設定で375 ns）は維持しています。
CH4のCCR=3998、ADC injectedの立上がりトリガも維持しています。

指定duty `d` は**相間の差動duty**です。正相のHigh-side dutyは `50% + d/2`、
負相は `50% - d/2`。両相のLow-sideはdead-timeを挟む相補PWMです。
例えば `d=0.02` なら正相51%、負相49%、`d=0.05` なら52.5%と47.5%。
片側を常時High/Lowへ固定する方式ではありません。

下表のH+は正相High-side PWM、H−は負相High-side PWM、L+/L−はそれぞれの相補PWMです。

| モード | sector | 正電圧区間の経路 | U High | U Low | V High | V Low | W High | W Low |
|---|---:|---|---|---|---|---|---|---|
| `uv_pos` | 1 | DC+ → U → 巻線 → V → DC− | H+ | L+ | H− | L− | OFF | OFF |
| `uv_neg` | 4 | DC+ → V → 巻線 → U → DC− | H− | L− | H+ | L+ | OFF | OFF |
| `vw_pos` | 3 | DC+ → V → 巻線 → W → DC− | OFF | OFF | H+ | L+ | H− | L− |
| `vw_neg` | 6 | DC+ → W → 巻線 → V → DC− | OFF | OFF | H− | L− | H+ | L+ |
| `wu_pos` | 5 | DC+ → W → 巻線 → U → DC− | H− | L− | OFF | OFF | H+ | L+ |
| `wu_neg` | 2 | DC+ → U → 巻線 → W → DC− | H+ | L+ | OFF | OFF | H− | L− |
| `off` | 0 | 駆動なし | OFF | OFF | OFF | OFF | OFF | OFF |

両相のHigh-sideがON、または両相のLow-sideがONの区間は相間電圧がほぼゼロで還流します。
正相High-sideと負相Low-sideがONの区間に正の相間電圧が生じ、平均は概ね `24 × d` Vです。
非通電相は両MOSFET OFFで端子Hi-Z。ただしボディダイオードの経路は残るので、
切替過渡中まで電流ゼロを保証するものではありません。

相変更・duty変更は一度MOEと全相CCERをOFFにし、既存bootstrap手順の後に再開します。
bootstrap中だけ全High-side OFF／全Low-side ONが750 µsあります。その後全相OFFで設定を復帰します。
通電時の比較値をプリロードへ書いた後UGを発行し、選んだ2相のみを有効にします。
同一相のHigh/Lowを独立に同時ONする設定は追加していません。
STSPIN32G4の入力プルダウン・interlocking・VDS保護は変更していません。
実機のゲート遅延・配線・MOSFET特性によるシュートスルー不発生は、オシロスコープで確認が必要です。
根拠: [STSPIN32G4データシート](https://www.st.com/resource/en/datasheet/stspin32g4.pdf)。

## 操作

USART1、115200 bps、8N1、CR/LF終端。既存コンソールを使用します。

```text
current_test off
current_test duty 0.002
current_test uv_pos
adc
```

`0.002`は0.2%。最初はこの程度以下から始めてください。
各行の応答を待って`adc`を送ります。時間経過による自動停止はなく、人力で入力できます。
4000組の取得は約0.2秒。その後は自動OFFになり、CSVを送ります。
UARTの受信待ち行列は既存の1行分なので、複数行を一括送信しないでください。

| コマンド | 動作 |
|---|---|
| `current_test uv_pos` / `uv_neg` / `vw_pos` / `vw_neg` / `wu_pos` / `wu_neg` | 通電相と方向を選択。保存済みdutyが正なら通電開始、ゼロならOFFを維持 |
| `current_test duty 0.02` | 差動dutyを2%に設定。相選択済みなら一度OFFにして再開。未選択なら値の保存のみ |
| `current_test duty 0` | PWMを停止。選択相は保持するため、次の正dutyで再開可能 |
| `current_test off` / `stop` | PWM停止、選択相とdutyをクリア。ADC取得・CSV送信中も受付 |
| `status` | 現在の動作モードと適用duty等を表示 |
| `adc` | 既存の4000組取得とCSV送信。OFF状態でも取得可能 |
| `fault` / `fault clear` | 既存fault診断／停止してクリア |

duty範囲は0～0.05、刻みは0.001（0.1%）で四捨五入。NaN、Inf、範囲外、余分な文字は拒否します。
不正コマンドは現在の状態を変えません。停止には必ず`off`または`stop`を使ってください。
取得／送信中は停止以外のコマンドを拒否し、mode/dutyの混在を防ぎます。
自動停止後はdutyと相選択がクリアされるため、測定のたびに再設定してください。
従来の`start`と`run`は残っていますが、静的評価には使用しません。

## 安全対策と限界

- 起動時は全相OFF・dutyゼロ。既存の起動時ADC校正も全相OFFで実行します。
- 時間経過による自動停止はありません。`adc`取得完了、`current_test off`／`stop`、または異常検出まで通電を継続します。
  測定を中断するときは手動で停止し、長時間通電や繰返しによる蓄熱に注意してください。
- nFAULT LowまたはMOE解除をTIM1更新割り込みで検出すると全相停止。
  STSPIN32G4のVDS・温度・低電圧保護設定と既存fault処理は維持しています。
- ADC開始／取得／同期／停止エラー、CPU例外、Error_Handlerでも停止。
  取得途中の停止は不完全なCSVを出さず、取得中断メッセージを出します。
- 静的通電のADC取得中はU1/V/U2/Wのいずれかが既存換算で5 Aを超えるraw差分なら停止。
  current値をログへ保存せず、raw-count領域で判定します。計測誤差・ノイズ・割り込み遅延は残ります。
- **ADCは`adc`取得中だけ動作する既存方式です。開始前・取得外にはソフトウェアの5 A監視はありません。**
  既存コードにも常時ADC過電流監視はありませんでした。VDS保護の閾値を5 Aへ変更したわけではありません。
- **TIM1 Break/Break2は元から無効**で、今回も設定を変更していません。
  nFAULTのソフトウェア監視はハードウェアbreakと同等ではありません。
  この機能を使うためのCubeMX変更は不要です。ハードウェアbreakを追加する場合は、内部接続・極性を別途確認してください。
- 5%では平均1.2 V。抵抗だけで5 A以下にするには相間抵抗が概ね0.24 Ω以上必要ですが、現状不明です。
  電源の制限だけで相電流5 Aは保証できません（還流中の相電流とDC入力電流は異なります）。
  最低duty・短時間から電流計／電流プローブで相電流を確認し、5 Aに余裕を持たせてください。
- 固定ベクトルでもロータは動き得ます。機械的に固定し、巻線・MOSFETの温度を確認します。
  NTCに温度停止機能を追加したものではありません。

## RAM、ADC対応、CSV

ログ本体は従来どおり **6 byte × 4000 = 24000 byte**。
U1/V/U2/Wの12 bit raw値だけを圧縮保存し、電流値の配列はありません。
既存sectorバッファ1500 byteも変更なし。modeポインタとdutyを取得ごとに1組だけ保持するため、
ログのメタデータは8 byte増えます。制御状態と出力行の作業領域も少量増えるため、総RAM増分ゼロではありません。

| 値 | ADC | OPAMP | 取得時点 |
|---|---|---|---|
| U1 | ADC1 injected rank 1 | OPAMP1 | Vと同時 |
| V | ADC2 injected rank 1 | OPAMP2 | U1と同時 |
| U2 | ADC1 injected rank 2 | OPAMP1 | Wと同時 |
| W | ADC2 injected rank 2 | OPAMP3 | U2と同時 |

**既存処理はU1/U2を別々に換算して出力しており、平均や片方への統合はしていません。** 今回も維持します。

```csv
sample,mode,duty,sector,u1_raw,v_raw,u2_raw,w_raw,u1_current,v_current,u2_current,w_current
```

`sample`は0～3999。modeは`off`または上記の6名称、従来駆動では`manual`/`six_step`。
dutyは比率（2%なら`0.020`）、OFFなら`0.000`。従来駆動では取得中一定でないため`nan`です。
rawは0～4095の整数、currentはAで小数3桁。既存CSVの列名と順序が変わります。
CSV生成時にだけ、保存rawから既存式 `(raw - 起動時の各rankオフセット) × A/count` で電流を算出します。
起動時VREFINT校正と共通係数 `VREF+/(4095 × 8 × 0.88 × 0.001)` を維持し、新しい相別ゲイン補正はありません。
保護用には5 AをADCカウント差へ換算して判定するだけで、電流ログは作りません。
CSV転送中はPWM OFFで、mode/dutyは取得時の値を出力します。115200 bpsで転送は概ね25～40秒です。

## 測定順序と評価

### Codex／PowerShellから6モードを自動取得

接続中の基板のCOMポートと測定dutyを指定すると、6モードを順番に取得して
モード別CSVへ保存できます。Windows標準の.NETシリアル機能を使うため、pyserialは不要です。

```powershell
powershell -ExecutionPolicy Bypass -File tools/capture_current_test.ps1 -Port COM11 -Duty 0.002
```

保存先は既定で`measurements/current_test_日時_duty_値/`です。
`uv_pos.csv`、`uv_neg.csv`、`vw_pos.csv`、`vw_neg.csv`、`wu_pos.csv`、`wu_neg.csv`を作ります。
各ファイルはヘッダーと4000行を検査してから確定し、不完全な取得は`.partial`のまま残します。
`-Repeat 3`で各モードを3回取得できます。処理終了・例外・Ctrl+C時には可能な限り
`current_test off`を送信しますが、PCやUSBの障害時に確実な停止を保証するものではありません。
基板へ書き込まれているファームウェアが本書のCSV形式と一致している必要があります。

1. 機械固定、相電流の測定系、電源制限、停止手段を用意。起動時のoffsetとA/countを保存。
2. `current_test off` → `adc`でOFFデータを保存。U1/V/U2/Wの平均rawをゼロ点Bとして求めます。
3. 十分冷却した状態で低dutyを設定し、`uv_pos` → 応答待ち → `adc`。取得後の自動OFFを確認。
4. dutyを再設定し`uv_neg`で同様に測定。続いて`vw_pos/vw_neg`、`wu_pos/wu_neg`。
5. 小刻みにdutyを増やし、同じ組合せを複数レベルで測定。5 Aより十分低く保ち、最大でも5%。
6. 各記録で立上がり・切替過渡を除き、定常部分の平均と標準偏差を計算。最後にOFFを再測定してドリフト確認。

offset差はOFF平均 `B_U1, B_U2, B_V, B_W` の差で比較します。
current列は既に起動時offsetを引いているので、OFFのcurrent平均は起動校正からの残差です。

各相の補正rawを `x = raw - B` とします。UVではU1とV、VWではVとW、WUではWとU2を比較。
UVW電流がセンサの正方向に対応するなら、同じ瞬間の直列相電流は `I_p = -I_q` なので、
複数レベルの定常平均で `x_p = a × (-x_q) + b` を回帰し、傾きaを相対ゲイン `G_p/G_q` と評価できます。
U1/VおよびU2/Wは同時取得なので優先します。V/Wはrank間の時間差によるリップル誤差も確認してください。
浮遊相は定常時ほぼゼロか、U1/U2間に時点依存の差がないかも確認します。

正逆で実電流の絶対値が同じなら、正逆raw平均の半和はoffset、半差はgain×電流の目安です。
同じdutyでも温度やdead-timeで電流が変わるため、この等電流条件を無条件に仮定しないでください。
絶対ゲイン校正には外部の基準電流測定が必要です。相対評価だけで自動補正係数を適用しません。

## 検証

GNU Arm 14.3.1でDebug／Releaseの両プリセットのビルド成功。
DebugはFLASH 88980 byte、Releaseは60372 byte、リンク時RAM使用量は両方29664/32768 byteです。
ELFのシンボルサイズからrawバッファ24000 byteとsectorバッファ1500 byteを確認しました。
既存版との関数比較でraw圧縮・展開、起動時scale/offset校正、TIM1/ADC初期化が不変であることも確認しました。

PC上のビルドとソース確認では実機波形や保護応答は確認できません。
実機ではまずOFF、duty=0、全6モードのゲート波形、非通電相OFF、dead-time、
相変更時のOFF区間、手動停止、取得中のoff、fault停止、5 A判定を段階的に確認してください。
最初から大電流でfaultを発生させず、電力段を制限した状態で検証してください。
