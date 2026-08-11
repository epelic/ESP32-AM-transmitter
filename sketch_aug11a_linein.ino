#include <Arduino.h>
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/i2s_std.h"

#define RF_PIN GPIO_NUM_27
#define AUDIO_PIN 34

// Configurazione I2S verificata: non modificare.
static constexpr uint32_t I2S_WORD_RATE = 90000;

static constexpr size_t WORDS = 9;
static constexpr uint32_t AUDIO_PERIOD_US = 100;
static constexpr float AUDIO_SAMPLE_RATE = 1000000.0f / AUDIO_PERIOD_US;

// Regolazioni iniziali prudenti.
static constexpr float AUDIO_GAIN = 8.0f;
static constexpr float CARRIER_LEVEL = 0.50f;
static constexpr float MODULATION_LEVEL = 0.475f;
static constexpr uint16_t AUDIO_QUEUE_SIZE = 512;
static constexpr uint16_t AUDIO_QUEUE_MASK = AUDIO_QUEUE_SIZE - 1;

i2s_chan_handle_t tx_handle = NULL;
adc_oneshot_unit_handle_t adc_handle = NULL;
esp_timer_handle_t audio_timer = NULL;
TaskHandle_t audio_task_handle = NULL;
uint16_t txBuffer[WORDS];

static uint32_t densityAccumulator = 0;
static bool suppressedPairHigh = false;

static float dcEstimate = 2048.0f;
static float audioFiltered = 0.0f;
static volatile uint16_t envelopeQ16 =
    (uint16_t)(CARRIER_LEVEL * 65535.0f);
static volatile uint32_t adcSampleCounter = 0;
static volatile int lastAdcRaw = 2048;
static volatile int adcMinimum = 4095;
static volatile int adcMaximum = 0;
static volatile uint32_t clippedSampleCounter = 0;
static uint32_t rfWordCounter = 0;
static volatile uint16_t audioQueue[AUDIO_QUEUE_SIZE];
static volatile uint16_t audioWriteIndex = 0;
static volatile uint16_t audioReadIndex = 0;
static volatile uint32_t audioDropCounter = 0;
static uint16_t currentEnvelopeQ16 =
    (uint16_t)(CARRIER_LEVEL * 65535.0f);

inline uint8_t nextRFPair(uint16_t requestedEnvelopeQ16)
{
  densityAccumulator += requestedEnvelopeQ16;

  if (densityAccumulator >= 65536UL)
  {
    densityAccumulator -= 65536UL;
    return 0b10;  // portante attiva, stessa fase di 0xAAAA
  }

  // Periodo RF soppresso; alternanza per mantenere bilanciato il DC.
  suppressedPairHigh = !suppressedPairHigh;
  return suppressedPairHigh ? 0b11 : 0b00;
}

void buildTxBuffer()
{
  uint16_t targetEnvelopeQ16 = currentEnvelopeQ16;
  uint16_t readIndex = audioReadIndex;

  if (readIndex != audioWriteIndex)
  {
    targetEnvelopeQ16 = audioQueue[readIndex];
    audioReadIndex = (readIndex + 1) & AUDIO_QUEUE_MASK;
  }

  const int32_t envelopeDelta =
      (int32_t)targetEnvelopeQ16 - (int32_t)currentEnvelopeQ16;
  uint32_t rfCycleNumber = 0;
  static constexpr uint32_t RF_CYCLES_IN_BLOCK = WORDS * 8;

  for (size_t i = 0; i < WORDS; i++)
  {
    uint16_t word = 0;

    for (int cycle = 0; cycle < 8; cycle++)
    {
      rfCycleNumber++;
      int32_t interpolated =
          (int32_t)currentEnvelopeQ16 +
          envelopeDelta * (int32_t)rfCycleNumber /
              (int32_t)RF_CYCLES_IN_BLOCK;
      word = (uint16_t)((word << 2) |
                        nextRFPair((uint16_t)interpolated));
    }

    txBuffer[i] = word;
  }

  currentEnvelopeQ16 = targetEnvelopeQ16;
}

void acquireAndProcessAudio()
{
  int rawValue = 2048;
  if (adc_oneshot_read(adc_handle, ADC_CHANNEL_6, &rawValue) != ESP_OK)
    return;
  float raw = (float)rawValue;
  lastAdcRaw = rawValue;
  if (rawValue < adcMinimum)
    adcMinimum = rawValue;
  if (rawValue > adcMaximum)
    adcMaximum = rawValue;

  // Servo DC lento: segue la polarizzazione, non il contenuto audio.
  // Costante di tempo circa 4 secondi a 10 kHz.
  dcEstimate += 0.000025f * (raw - dcEstimate);

  float audio = (raw - dcEstimate) / 2048.0f;
  audio *= AUDIO_GAIN;

  if (audio <= -1.0f || audio >= 1.0f)
    clippedSampleCounter++;

  // Passa-basso a circa 4.2 kHz (-3 dB) con Fs = 10 kHz.
  audioFiltered += 0.82057f * (audio - audioFiltered);
  audioFiltered = constrain(audioFiltered, -1.0f, 1.0f);

  float envelope =
      CARRIER_LEVEL + MODULATION_LEVEL * audioFiltered;
  envelope = constrain(envelope, 0.0f, 1.0f);
  uint16_t newEnvelopeQ16 = (uint16_t)(envelope * 65535.0f + 0.5f);
  envelopeQ16 = newEnvelopeQ16;

  uint16_t writeIndex = audioWriteIndex;
  uint16_t nextWriteIndex = (writeIndex + 1) & AUDIO_QUEUE_MASK;
  if (nextWriteIndex != audioReadIndex)
  {
    audioQueue[writeIndex] = newEnvelopeQ16;
    audioWriteIndex = nextWriteIndex;
  }
  else
  {
    audioDropCounter++;
  }
  adcSampleCounter++;
}

void audioTask(void *parameter)
{
  for (;;)
  {
    // Le notifiche accumulate vengono accorpate: niente raffiche di letture
    // arretrate se occasionalmente il task viene servito in ritardo.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    acquireAndProcessAudio();
  }
}

void audioTimerCallback(void *argument)
{
  // Il callback non accede all'ADC e non genera RF.
  xTaskNotifyGive(audio_task_handle);
}

void setupADC()
{
  adc_oneshot_unit_init_cfg_t unitConfig = {
    .unit_id = ADC_UNIT_1,
    .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
    .ulp_mode = ADC_ULP_MODE_DISABLE
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unitConfig, &adc_handle));

  adc_oneshot_chan_cfg_t channelConfig = {
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_DEFAULT
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(
      adc_handle, ADC_CHANNEL_6, &channelConfig));
}

void setupI2S()
{
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(
          I2S_NUM_0,
          I2S_ROLE_MASTER
      );

  chan_cfg.dma_desc_num  = 8;
  chan_cfg.dma_frame_num = 256;

  ESP_ERROR_CHECK(
      i2s_new_channel(
          &chan_cfg,
          &tx_handle,
          NULL
      )
  );

  i2s_std_config_t std_cfg = {

    .clk_cfg =
        I2S_STD_CLK_DEFAULT_CONFIG(
            90000
        ),

    .slot_cfg =
        I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_MONO
        ),

    .gpio_cfg = {

        .mclk = I2S_GPIO_UNUSED,
        .bclk = I2S_GPIO_UNUSED,
        .ws   = I2S_GPIO_UNUSED,

        .dout = RF_PIN,
        .din  = I2S_GPIO_UNUSED,

        .invert_flags = {
            .mclk_inv = false,
            .bclk_inv = false,
            .ws_inv   = false
        }
    }
  };

  ESP_ERROR_CHECK(
      i2s_channel_init_std_mode(
          tx_handle,
          &std_cfg
      )
  );

  ESP_ERROR_CHECK(
      i2s_channel_enable(
          tx_handle
      )
  );
}

void setup()
{
  Serial.begin(115200);
  delay(500);

  setupADC();

  // Stabilizzazione iniziale della stima DC prima di avviare la RF.
  uint32_t sum = 0;
  for (int i = 0; i < 64; i++)
  {
    int raw = 2048;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL_6, &raw));
    sum += raw;
  }
  dcEstimate = (float)sum / 64.0f;

  BaseType_t taskResult = xTaskCreatePinnedToCore(
      audioTask,
      "audio-adc",
      3072,
      NULL,
      2,
      &audio_task_handle,
      0
  );
  if (taskResult != pdPASS)
  {
    Serial.println("ERRORE: creazione task audio fallita");
    abort();
  }

  esp_timer_create_args_t timerConfig = {
    .callback = &audioTimerCallback,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "audio-clock",
    .skip_unhandled_events = true
  };
  ESP_ERROR_CHECK(esp_timer_create(&timerConfig, &audio_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(audio_timer, AUDIO_PERIOD_US));

  // Precarica circa 25 ms di campioni cronologici. Questa riserva compensa la
  // profondita' della coda DMA, che inizialmente accetta diversi blocchi senza
  // bloccarsi.
  while (((audioWriteIndex - audioReadIndex) & AUDIO_QUEUE_MASK) < 256)
    delay(1);

  buildTxBuffer();
  setupI2S();

  Serial.println();
  Serial.println("=============================");
  Serial.println(" ESP32 AM - GPIO34 LINE IN");
  Serial.println("=============================");
  Serial.println("RF          : GPIO27, 1440 kHz");
  Serial.println("Audio       : GPIO34, bias ~1.65 V");
  Serial.println("Audio Fs    : 10 kHz one-shot");
  Serial.println("Audio gain  : 8.0");
  Serial.println("Modulation  : 0.50 +/- 0.475 (95%)");
  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());
  Serial.println("DIAG: ADC/s, RF words/s, ADC raw, DC");
}

void loop()
{
  size_t written = 0;

  ESP_ERROR_CHECK(
      i2s_channel_write(
          tx_handle,
          txBuffer,
          sizeof(txBuffer),
          &written,
          portMAX_DELAY
      )
  );

  // Il livello audio viene aggiornato dal task a cadenza reale di 10 kHz.
  buildTxBuffer();
  rfWordCounter += WORDS;

  static uint32_t lastReportMs = 0;
  static uint32_t lastAdcCount = 0;
  static uint32_t lastRfCount = 0;
  static uint32_t lastClipCount = 0;
  static uint32_t lastDropCount = 0;
  uint32_t nowMs = millis();

  if (nowMs - lastReportMs >= 1000)
  {
    uint32_t adcNow = adcSampleCounter;
    uint32_t rfNow = rfWordCounter;
    uint32_t clipNow = clippedSampleCounter;
    uint32_t dropNow = audioDropCounter;
    int minimum = adcMinimum;
    int maximum = adcMaximum;
    adcMinimum = 4095;
    adcMaximum = 0;
    Serial.printf(
        "DIAG: ADC=%lu/s RF=%lu/s MIN=%d MAX=%d DC=%.1f CLIP=%lu/s DROP=%lu/s ENV=%u\n",
        (unsigned long)(adcNow - lastAdcCount),
        (unsigned long)(rfNow - lastRfCount),
        minimum,
        maximum,
        dcEstimate,
        (unsigned long)(clipNow - lastClipCount),
        (unsigned long)(dropNow - lastDropCount),
        (unsigned int)envelopeQ16);

    lastAdcCount = adcNow;
    lastRfCount = rfNow;
    lastClipCount = clipNow;
    lastDropCount = dropNow;
    lastReportMs = nowMs;
  }
}

