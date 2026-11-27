// sound.h — the "Sound" screen (Settings -> Sound): the speaker's volume (10 % a step) and the microphone's gain
// (3 dB a step), on the Display screen's sliders (display.h). A change is heard at once (oriole_audio_volume,
// oriole_mic_gain) and saved as the volume and mic_gain settings (config.h: cfg_ui_set — the same ones the web
// controller sets). Included from Oriole.ino after display.h.

// Two pills (104 px tall) and their labels in the circle: the lower pill's round ends just inside the edge
#define SN_HEAD_Y 62.0f
#define SN_VOL_LABEL_Y 120.0f
#define SN_VOL_Y 190.0f
#define SN_MIC_LABEL_Y 258.0f
#define SN_MIC_Y 326.0f  // its round ends 231 px from the middle: inside the edge (233)

static Slider &sn_vol = psram_new<Slider>(), &sn_mic = psram_new<Slider>();  // in PSRAM
static char sn_vol_s[24], sn_mic_s[32];
static float sn_mid_head, sn_mid_label;
static OrioleItem sn_head, sn_vol_label, sn_mic_label;

static void sound_build() {
  sn_mid_head = oriole_text_mid(t_title, "Sound");
  sn_mid_label = oriole_text_mid(t20, "Volume 0123456789%");
}

static void sound_enter() {  // the settings as they are now (the controller may have changed them)
  slider_init(sn_vol, SN_VOL_Y, 10, (cfg.vol + 5) / 10);
  slider_init(sn_mic, SN_MIC_Y, 12, (cfg.gain + 1) / 3);
  slider_enter(sn_vol), slider_enter(sn_mic);
}

static void sound_update(float dt) {
  const int v = sn_vol.value, m = sn_mic.value;
  slider_update(sn_vol, dt), slider_update(sn_mic, dt);
  slider_tick(sn_vol, dt), slider_tick(sn_mic, dt);
  if (sn_vol.value != v) oriole_audio_volume(sn_vol.value * 10), cfg_ui_set("volume", sn_vol.value * 10);
  if (sn_mic.value != m) oriole_mic_gain(sn_mic.value * 3), cfg_ui_set("mic_gain", sn_mic.value * 3);
}

static void sound_layout(const OrioleView &v) {
  snprintf(sn_vol_s, sizeof(sn_vol_s), "Volume %d%%", sn_vol.value * 10);
  snprintf(sn_mic_s, sizeof(sn_mic_s), "Microphone %d dB", sn_mic.value * 3);
  text_mid(sn_head, t_title, "Sound", sn_mid_head, CX, SN_HEAD_Y, 1, TEXT, 1, &oriole_black);
  text_mid(sn_vol_label, t20, sn_vol_s, sn_mid_label, CX, SN_VOL_LABEL_Y, 1, DIM, 1, &oriole_black);
  text_mid(sn_mic_label, t20, sn_mic_s, sn_mid_label, CX, SN_MIC_LABEL_Y, 1, DIM, 1, &oriole_black);
  sn_head.on_black = sn_vol_label.on_black = sn_mic_label.on_black = true;
  oriole_view_commit(sn_head, v), oriole_view_commit(sn_vol_label, v), oriole_view_commit(sn_mic_label, v);
  slider_layout(sn_vol, v), slider_layout(sn_mic, v);
}

static void sound_draw() {
  oriole_item_draw(sn_head), oriole_item_draw(sn_vol_label), oriole_item_draw(sn_mic_label);
  slider_draw(sn_vol), slider_draw(sn_mic);
}

static const OrioleScreen SOUND_SCREEN = {sound_enter, sound_update, sound_layout, sound_draw, false, ORIOLE_HOME_BOTH};
