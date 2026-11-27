// noise.h — the Noise app, like Apple Watch's: how loud it is around you, from the microphones, while the screen is up.
// The level 8 times a second (the mic task), the minute's average, and a ding with a warning once the last 3 minutes
// averaged 80 dB or more (hearing damage starts with hours at 80, minutes at 100). Included after apps.h.
//
// shortcut: dB SPL = dBFS + NM_CAL − the mic gain, with NM_CAL a typical MEMS mic's (94 dB SPL ≈ −26 dBFS), not
// measured against a sound meter: readings are a guide (±5 dB or so); calibrate NM_CAL with a real meter to trust them

#define NM_CAL 120.0f
#define NM_LO 30.0f   // the ring: 30 dB empty ...
#define NM_HI 110.0f  // ... 110 dB full
static volatile float nm_dbfs = -90;  // the last 1/8 s (the mic task)
static volatile uint32_t nm_ver;
static volatile bool nm_on;
static uint32_t nm_seen_ms;
static int nm_nsec, nm_head;
static float nm_acc;
static int nm_acc_n;
static uint32_t nm_tick_ms, nm_last_ver, nm_from_ms;
static bool nm_warned, nm_got;  // nm_got: a level came since the screen opened
static float nm_db = 0, nm_avg1 = 0, nm_avg3 = 0;
static OrioleSpring nm_ring;
struct NmUi {  // in PSRAM
  float sec[180];  // energy (linear) of each of the last 3 minutes' seconds
  OrioleItem head, ring, big, unit, word, line;
  char big_s[8], line_s[48];
};
static NmUi &nm = psram_new<NmUi>();
#define nm_sec nm.sec
#define nm_head_it nm.head
#define nm_ring_it nm.ring
#define nm_big nm.big
#define nm_unit nm.unit
#define nm_word nm.word
#define nm_line nm.line
#define nm_big_s nm.big_s
#define nm_line_s nm.line_s
static float nm_mid_big;

static void nm_sink(const int16_t *b, int n) {  // the mic task: DC removed (as the assistant's), energy summed
  static float x1, y1, e;
  static int k;
  for (int i = 0; i < n; i++) {
    const float y = b[i] - x1 + 0.995f * y1;
    x1 = b[i], y1 = y, e += y * y;
  }
  k += n;
  const int rate = oriole_audio_rate() ? oriole_audio_rate() : 16000;
  if (k < rate / 8) return;
  nm_dbfs = 10 * log10f(e / k / (32768.0f * 32768.0f) + 1e-12f), nm_ver++;
  e = 0, k = 0;
}

static float nm_spl(float dbfs) {
  return dbfs + NM_CAL - cfg.gain;
}

static void noise_tick() {  // every frame: the screen gone — the mics off (unless the assistant took them)
  if (nm_on && oriole_ms() - nm_seen_ms > 300) {
    nm_on = false;
    if (!ai_hold && !ai_capture) oriole_mic_off();
  }
}

static void noise_enter() {
  nm_nsec = nm_head = 0, nm_acc = 0, nm_acc_n = 0, nm_warned = false, nm_tick_ms = millis();
  nm_db = nm_avg1 = nm_avg3 = 0, nm_got = false, nm_last_ver = nm_ver;
  oriole_spring_set(nm_ring, 0);
}

static void noise_update(float dt) {
  nm_seen_ms = oriole_ms();
  if (!ai_hold && !ai_capture && (!nm_on || !oriole_mic_is_on()))  // (after Gemini too)
    oriole_mic_on(nm_sink), nm_on = true, nm_from_ms = millis() + 500;  // the mics' first half second: a thump
  if (nm_ver != nm_last_ver) {
    nm_last_ver = nm_ver;
    if ((int32_t)(millis() - nm_from_ms) < 0) return;
    nm_got = true;
    nm_db = fmaxf(0, nm_spl(nm_dbfs));
    nm_acc += powf(10, nm_db / 10), nm_acc_n++;
  }
  if (millis() - nm_tick_ms >= 1000 && nm_acc_n) {  // a second more: into the 3 minutes
    nm_tick_ms += 1000;
    nm_sec[nm_head] = nm_acc / nm_acc_n, nm_head = (nm_head + 1) % 180, nm_nsec = min(nm_nsec + 1, 180);
    nm_acc = 0, nm_acc_n = 0;
    double e1 = 0, e3 = 0;
    const int n1 = min(nm_nsec, 60);
    for (int i = 0; i < nm_nsec; i++) {
      const float v = nm_sec[(nm_head + 179 - i) % 180];
      e3 += v;
      if (i < n1) e1 += v;
    }
    nm_avg1 = 10 * log10(e1 / n1), nm_avg3 = 10 * log10(e3 / nm_nsec);
    if (nm_nsec == 180 && nm_avg3 >= 80 && !nm_warned) {
      nm_warned = true;
      if ((int32_t)(millis() - ai_chime_until) >= 0) ai_chime_mode = 1, ai_chime_until = millis() + 300;
      LOG("noise: loud for 3 minutes (%.0f dB)\n", nm_avg3);
    }
    if (nm_avg3 < 75) nm_warned = false;
  }
  nm_ring.target = oriole_clamp01((nm_db - NM_LO) / (NM_HI - NM_LO));
  oriole_spring_run(nm_ring, K_M3_DEFAULT, dt);
}

static uint32_t nm_color(float db) {
  return db < 70 ? 0x5CF08A : db < 85 ? 0xFDD663 : 0xF28B82;
}

static void noise_layout(const OrioleView &v) {
  const float db = nm_db;
  const uint32_t c = nm_color(db);
  text_mid(nm_head_it, t_title, "Noise", ap_mid_head, CX, AP_HEAD_Y, 1, TEXT, 1, &oriole_black);
  nm_ring_it.kind = HI_RING, nm_ring_it.x = CX, nm_ring_it.y = CX, nm_ring_it.r = 214, nm_ring_it.w = 14;
  nm_ring_it.scale = 1, nm_ring_it.v = oriole_clamp01(nm_ring.x), nm_ring_it.color = c, nm_ring_it.color2 = 0x2A2C30;
  nm_ring_it.alpha = 1, nm_ring_it.on_black = true;
  snprintf(nm_big_s, sizeof(nm_big_s), "%d", (int)lroundf(db));
  text_mid(nm_big, n48, nm_big_s, nm_mid_big, CX, 196, 1.6f, TEXT, 1, &oriole_black);
  text_mid(nm_unit, t24, "dB", ap_mid_t24, CX, 262, 1, DIM, 1, &oriole_black);
  const char *w = !nm_got ? "Listening..." : db < 50 ? "Quiet" : db < 70 ? "Moderate" : db < 85 ? "Loud" : "Very loud";
  text_mid(nm_word, t24, w, ap_mid_t24, CX, 314, 1, c, 1, &oriole_black);
  if (nm_warned) snprintf(nm_line_s, sizeof(nm_line_s), "Loud for 3 min: protect your ears");
  else if (nm_nsec >= 5) snprintf(nm_line_s, sizeof(nm_line_s), "%s average %d dB", nm_nsec >= 60 ? "1 min" : "So far", (int)lroundf(nm_avg1));
  else nm_line_s[0] = 0;
  text_mid(nm_line, t20, nm_line_s, ap_mid_t20, CX, 356, 1, nm_warned ? 0xF28B82 : DIM, 1, &oriole_black);
  for (OrioleItem *it : {&nm_head_it, &nm_ring_it, &nm_big, &nm_unit, &nm_word, &nm_line})
    it->on_black = true, oriole_view_commit(*it, v);
}

static void noise_draw() {
  for (OrioleItem *it : {&nm_ring_it, &nm_head_it, &nm_big, &nm_unit, &nm_word, &nm_line}) oriole_item_draw(*it);
}

static const OrioleScreen NOISE_SCREEN = {noise_enter, noise_update, noise_layout, noise_draw, false, ORIOLE_HOME_BOTH};

static void noise_build() {
  nm_mid_big = oriole_text_mid(n48, "0123456789");
}
