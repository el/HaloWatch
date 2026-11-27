// Звук для Oriole: динамик через кодек ES8311 (I2C 0x18 — на шине тача) и усилитель (GPIO46); микрофоны — через
// АЦП ES7210 (I2C 0x40) на той же шине I2S.
//
// I2S (драйвер ESP-IDF i2s_std): ESP32 ведёт шину — MCLK = 256·fs на GPIO16, BCLK 9, LRCK 45, данные 8; 16 бит,
// стерео (в оба канала — одно и то же: динамик один). Кодек — ведомый, тактируется от MCLK. Регистры ES8311 — из
// драйвера Espressif (es8311.c, Apache-2.0; он же в lib/AmoledAudio): сброс, тактирование для MCLK = 256·fs
// (одинаковое для 16 000…48 000 Гц), формат 16 бит, питание ЦАП и выхода, громкость — регистр 0x32.
//
// Звук даёт скетч: задача звука (ядро 0) зовёт fill(buf, n) — n отсчётов моно; вернул меньше n — дальше тишина.
// Пока тишина дольше 150 мс — усилитель выключен (динамик не шипит), задача спит, а DMA сам шлёт нули. Буфер DMA —
// 6 × 256 кадров (~70 мс при 22 050 Гц): запись во флеш (настройки) на миг останавливает задачи из флеша, звук
// за это время доигрывает из буфера.
//
// Microphones: the ES7210 sits on the same I2S bus as the ES8311 (MCLK, BCLK and LRCK shared, its data on GPIO10), so
// the channel is full duplex — one I2S controller, a TX and an RX channel created together, one sample rate for both.
// The ES7210 is a slave too; in plain I2S mode its SDOUT1 carries ADC1 and ADC2 (MIC1 left, MIC2 right). With
// MCLK = 256·fs its clock dividers are the same for any rate (Espressif's table: adc_div 1, doubler, DLL, OSR 0x20,
// LRCK = MCLK/256). Register sequence — Waveshare's es7210.cpp for this board (Apache-2.0). Capturing runs in a task
// of its own (core 0): oriole_mic_on(sink) — the sink gets mono 16-bit blocks of 256 samples, oriole_mic_off() — the RX
// channel stops and the ES7210's microphones and ADCs power down.
#include "oriole_internal.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#define FRAMES 256
static i2s_chan_handle_t tx, rx;
static i2c_master_dev_handle_t es, em;  // ES8311, ES7210
static OrioleAudioFill fill_cb;
static int rate;
static volatile int volume = 60;
static volatile bool vol_dirty;
static volatile bool pa_on;            // the amplifier is on: a DMA underrun now is heard
static volatile uint32_t underruns;    // DMA ran out of samples while playing (the task was late)

static bool IRAM_ATTR on_underrun(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  if (pa_on) underruns++;
  return false;
}

uint32_t oriole_audio_underruns() {
  return underruns;
}


static void es_w(uint8_t r, uint8_t v) {
  const uint8_t b[2] = {r, v};
  i2c_master_transmit(es, b, 2, 20);
}

static uint8_t es_r(uint8_t r) {
  uint8_t v = 0;
  i2c_master_transmit_receive(es, &r, 1, &v, 1, 20);
  return v;
}

static uint8_t vol_reg(int v) {  // 0..100 → регистр 0x32 (как es8311_voice_volume_set: 0 — тишина, 100 — +32 дБ)
  return v <= 0 ? 0 : (uint8_t)(v * 256 / 100 - 1);
}

static void audio_task(void *) {
  static int16_t mono[FRAMES], st[FRAMES * 2];
  bool pa = false;
  int silent = 1 << 30;  // отсчётов тишины подряд
  for (;;) {
    if (vol_dirty) vol_dirty = false, es_w(0x32, vol_reg(volume));
    const int n = fill_cb ? fill_cb(mono, FRAMES) : 0;
    if (n <= 0) {
      silent += FRAMES;
      if (pa && silent > rate * 15 / 100) gpio_set_level((gpio_num_t)ORIOLE_PIN_PA, 0), pa = pa_on = false;
      if (!pa) {  // тихо: DMA сам шлёт нули (auto_clear), задаче — спать
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
    } else {
      silent = 0;
      if (!pa) gpio_set_level((gpio_num_t)ORIOLE_PIN_PA, 1), pa = pa_on = true;
    }
    for (int i = 0; i < FRAMES; i++) st[2 * i] = st[2 * i + 1] = i < n ? mono[i] : 0;
    size_t w = 0;
    i2s_channel_write(tx, st, sizeof(st), &w, pdMS_TO_TICKS(200));
  }
}

bool oriole_audio_begin(OrioleAudioFill fill, int sample_rate) {
  if (tx) return true;
  es = ht_i2c_device(0x18);
  if (!es) {
    LOG("audio: ES8311 NOT FOUND\n");
    return false;
  }
  rate = sample_rate, fill_cb = fill;
  gpio_set_direction((gpio_num_t)ORIOLE_PIN_PA, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)ORIOLE_PIN_PA, 0);
  // I2S: сначала такты (кодеку нужен MCLK, чтобы принять настройки)
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6, cc.dma_frame_num = FRAMES, cc.auto_clear = true;
  if (i2s_new_channel(&cc, &tx, &rx) != ESP_OK) return LOG("audio: I2S FAILED\n"), tx = rx = nullptr, false;
  i2s_std_config_t sc = {};
  sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate);
  sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = (gpio_num_t)ORIOLE_PIN_MCLK, sc.gpio_cfg.bclk = (gpio_num_t)ORIOLE_PIN_BCLK;
  sc.gpio_cfg.ws = (gpio_num_t)ORIOLE_PIN_LRCK, sc.gpio_cfg.dout = (gpio_num_t)ORIOLE_PIN_DOUT;
  sc.gpio_cfg.din = I2S_GPIO_UNUSED;
  i2s_channel_init_std_mode(tx, &sc);
  sc.gpio_cfg.dout = I2S_GPIO_UNUSED, sc.gpio_cfg.din = (gpio_num_t)ORIOLE_PIN_DIN;  // the same clocks, the mics' data
  if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK) i2s_del_channel(rx), rx = nullptr;
  const i2s_event_callbacks_t cb = {nullptr, nullptr, nullptr, on_underrun};
  i2s_channel_register_event_callback(tx, &cb, nullptr);
  i2s_channel_enable(tx);
  // ES8311: сброс и включение, MCLK с вывода, все такты; делители для MCLK = 256·fs (pre_div 1, ×1, adc/dac div 1,
  // одинарная скорость, LRCK = MCLK/256, BCLK = MCLK/4, OSR 0x10)
  es_w(0x00, 0x1F), es_w(0x00, 0x00), es_w(0x00, 0x80);
  es_w(0x01, 0x3F);
  es_w(0x06, es_r(0x06) & ~0x20);                // BCLK не инвертирован
  es_w(0x02, es_r(0x02) & 0x07);                 // pre_div 1, pre_multi ×1
  es_w(0x03, 0x10), es_w(0x04, 0x10), es_w(0x05, 0x00);
  es_w(0x06, (es_r(0x06) & 0xE0) | 0x03);        // BCLK = MCLK / 4
  es_w(0x07, es_r(0x07) & 0xC0), es_w(0x08, 0xFF);  // LRCK = MCLK / 256
  es_w(0x00, es_r(0x00) & 0xBF);                 // ведомый
  es_w(0x09, 0x0C), es_w(0x0A, 0x0C);            // 16 бит на входе и выходе
  es_w(0x0D, 0x01), es_w(0x0E, 0x02), es_w(0x12, 0x00), es_w(0x13, 0x10);  // питание: аналог, ЦАП, выход
  es_w(0x1C, 0x6A), es_w(0x37, 0x08);            // без эквалайзеров
  es_w(0x32, vol_reg(volume));
  xTaskCreatePinnedToCore(audio_task, "oriole_audio", 3072, nullptr, 5, nullptr, 0);
  LOG("audio: ES8311 ok, %d Hz\n", rate);
  return true;
}

void oriole_audio_volume(int v) {
  volume = constrain(v, 0, 100), vol_dirty = true;
}

int oriole_audio_get_volume() {
  return volume;
}

int oriole_audio_rate() {
  return tx ? rate : 0;
}

// ---------- Microphones: ES7210 ----------
static volatile OrioleMicSink mic_sink;
static volatile bool mic_want, mic_dirty = true;
static volatile int mic_gain = 30, mic_ch;
static TaskHandle_t mic_task_h;

static void em_w(uint8_t r, uint8_t v) {
  const uint8_t b[2] = {r, v};
  i2c_master_transmit(em, b, 2, 20);
}

static uint8_t em_r(uint8_t r) {
  uint8_t v = 0;
  i2c_master_transmit_receive(em, &r, 1, &v, 1, 20);
  return v;
}

static uint8_t em_gain_code(int db) {  // 0..14: 0, 3, … 33 dB, then 34.5, 36, 37.5
  return db >= 37 ? 14 : db >= 36 ? 13 : db >= 34 ? 12 : (uint8_t)constrain(db / 3, 0, 11);
}

static void em_init() {  // slave, I2S 16 bit, MCLK = 256·fs, MIC1 and MIC2
  em_w(0x00, 0xFF), em_w(0x00, 0x41);  // reset
  em_w(0x01, 0x1F);                    // all clocks off while setting up
  em_w(0x09, 0x30), em_w(0x0A, 0x30);  // chip state and power-on cycles
  em_w(0x40, 0xC3);                    // analog: VDDA 3.3 V, VMID 5 kΩ start
  em_w(0x41, 0x70), em_w(0x42, 0x70);  // mic bias 2.87 V
  em_w(0x07, 0x20);                    // OSR
  em_w(0x02, 0xC1);                    // adc_div 1, clock doubler, DLL
  em_w(0x04, 0x01), em_w(0x05, 0x00);  // LRCK = MCLK / 256
  em_w(0x11, (em_r(0x11) & 0x1C) | 0x60);  // 16 bits, I2S normal
  em_w(0x12, 0x00);                    // ADC1/2 -> SDOUT1
  for (int i = 0; i < 4; i++) em_w(0x43 + i, em_r(0x43 + i) & ~0x10);
  em_w(0x4B, 0xFF), em_w(0x4C, 0xFF);  // MIC1/2 and MIC3/4 powered down
}

static void em_start() {
  em_w(0x01, 0x14);                    // clocks on for ADC1/2 (0x1F with 0x0B cleared)
  em_w(0x06, 0x00);                    // power up
  em_w(0x47, 0x00), em_w(0x48, 0x00);  // MIC1, MIC2 power
  em_w(0x4B, 0x00);                    // MIC1/2 bias and ADC
  const uint8_t g = 0x10 | em_gain_code(mic_gain);
  em_w(0x43, g), em_w(0x44, g);
}

static void em_stop() {
  em_w(0x47, 0xFF), em_w(0x48, 0xFF), em_w(0x49, 0xFF), em_w(0x4A, 0xFF);
  em_w(0x4B, 0xFF), em_w(0x4C, 0xFF);
  em_w(0x01, 0x7F), em_w(0x06, 0x07);
}

static void mic_task(void *) {
  static int16_t st[FRAMES * 2], mono[FRAMES];
  bool on = false;
  em_init();
  for (;;) {
    const bool want = mic_want;
    if (want != on) {
      on = want;
      if (on) em_start(), i2s_channel_enable(rx);
      else i2s_channel_disable(rx), em_stop();
      mic_dirty = false;
      LOG("mic: %s\n", on ? "on" : "off");
    }
    if (on && mic_dirty) {
      mic_dirty = false;
      const uint8_t g = 0x10 | em_gain_code(mic_gain);
      em_w(0x43, g), em_w(0x44, g);
    }
    if (!on) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
      continue;
    }
    size_t got = 0;
    if (i2s_channel_read(rx, st, sizeof(st), &got, pdMS_TO_TICKS(100)) != ESP_OK || got < 4) continue;
    const int n = got / 4, ch = mic_ch;
    for (int i = 0; i < n; i++) {
      const int l = st[2 * i], r = st[2 * i + 1];
      mono[i] = ch == 1 ? l : ch == 2 ? r : (l + r) / 2;
    }
    const OrioleMicSink sink = mic_sink;
    if (sink) sink(mono, n);
  }
}

bool oriole_mic_on(OrioleMicSink sink) {
  if (!tx || !rx) return false;
  if (!em) {
    em = ht_i2c_device(0x40);
    if (!em) return LOG("mic: ES7210 NOT FOUND\n"), false;
  }
  mic_sink = sink, mic_want = true;
  if (!mic_task_h) xTaskCreatePinnedToCore(mic_task, "oriole_mic", 3072, nullptr, 5, &mic_task_h, 0);
  else xTaskNotifyGive(mic_task_h);
  return true;
}

void oriole_mic_off() {
  mic_want = false;
  if (mic_task_h) xTaskNotifyGive(mic_task_h);
}

bool oriole_mic_is_on() {
  return mic_want;
}

void oriole_mic_gain(int db) {
  mic_gain = constrain(db, 0, 37), mic_dirty = true;
}

void oriole_mic_channel(int ch) {
  mic_ch = constrain(ch, 0, 2);
}
