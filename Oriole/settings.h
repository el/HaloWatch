// settings.h — the "Settings" screen: a list of sections as the gallery's cards ("Watch face": its style, the clock's
// color and 12/24 hours, face.h; "Display": the screen's brightness, display.h; "Sound": volume and the microphone,
// sound.h; "Wi-Fi": the network, its status, signal and address; "Battery": the level and its history, battery.h; "Device": restart, turn off). A tap opens the section
// right over the settings (halo_nav_open from an open screen; back — swipe from the left edge, to the face — down from
// the top or up from the bottom edge). Watch face and Device are lists of the same cards (CardList): a tap on a row
// steps its value (saved as a setting, config.h: cfg_ui_set) or, tapped twice within 3 s, restarts or turns off.
//
// What is set here is kept in flash (NVS, namespace "halowatch": the brightness — "bright", the lock — "lock"): read in
// setup() before halo_begin (the panel starts at the saved brightness, locked or not), written from loop() (a flash
// write stalls the other core for a moment — not in the middle of a frame); the brightness 0.8 s after the last change
// (not on every step of a drag), the lock at once.
//
// The transition to the Weather screen used to be chosen here (Zoom, Fade, Slide; radio buttons and "Try it") — now it
// is Fade, fixed (docs/halo.md, section 10; the command fx N still switches it until a reboot, to compare).
// Included from Oriole.ino after display.h and battery.h.
#include <Preferences.h>

static Preferences se_prefs;
static int se_saved = -1, se_seen = -1;  // the brightness in flash; the last one seen and when it changed
static uint32_t se_changed_at;

static bool se_lock_saved;

static void settings_load() {
  se_prefs.begin("halowatch", false);  // the project's old name, kept: a new one would lose what is saved
  se_saved = se_seen = constrain((int)se_prefs.getUChar("bright", DP_DEFAULT), 0, DP_STEPS);
  slider_init(dp_sl, DP_SL_Y, DP_STEPS, se_saved);
  dp_locked = se_lock_saved = se_prefs.getBool("lock", false);
}
static void settings_save_poll(bool now = false) {  // from loop(); now — at once (before a restart)
  if (dp_locked != se_lock_saved) se_lock_saved = dp_locked, se_prefs.putBool("lock", se_lock_saved);  // at once
  const int v = dp_sl.value;
  if (v != se_seen) se_seen = v, se_changed_at = millis();
  if (v != se_saved && (now || millis() - se_changed_at > 800)) {
    se_prefs.putUChar("bright", v), se_saved = v;
    LOG("brightness %d/%d (%d) saved\n", v, DP_STEPS, DP_LEVEL[v]);
  }
}

// ---------- A list of cards (the gallery's look): a header, rows with an optional icon disc, a title and a subtitle ----------
struct CardRow {
  const char *title, *icon;  // the icon — Font Awesome (ic24), or nullptr: no disc
  uint32_t color;
  const char *(*sub)();      // the subtitle, asked every frame (a value that changes)
  void (*tap)();
  // Rows made at run time (History, World clock): the same three by the row's index, in place of the ones above
  const char *(*title_i)(int);
  const char *(*sub_i)(int);
  void (*tap_i)(int);
};
#define CL_MAX 20
struct CardList {
  const char *head;
  const CardRow *rows;
  int n;
  HaloList list;
  Press press[CL_MAX];
  int armed;
  float mid_head;
  HaloItem head_it, bg[CL_MAX], disc[CL_MAX], icon[CL_MAX], title[CL_MAX], sub[CL_MAX];
  HaloItem wifi[CL_MAX][5];  // CL_WIFI / CL_BT rows: the symbol drawn with arcs and lines (the icon font has none)
};
static float cl_mid_icon;
static const char CL_WIFI[] = "wifi", CL_BT[] = "bt";  // as a row's icon: the Wi-Fi or the Bluetooth symbol

static void cl_build(CardList &c) {
  float h[CL_MAX + 1];
  h[0] = M_HEAD_H;
  for (int i = 0; i < c.n; i++) h[i + 1] = M_ITEM_H;
  halo_list_init(c.list, h, c.n + 1, 64, 330, true);
  c.mid_head = halo_text_mid(t_title, c.head);
  cl_mid_icon = halo_text_mid(ic24, "");
}

static void cl_enter(CardList &c) {  // from the top
  halo_list_reset(c.list);
  c.armed = -1;
  for (int i = 0; i < c.n; i++) halo_spring_set(c.press[i].s, 0), c.press[i].going_down = false;
}

static void cl_update(CardList &c, float dt) {
  HaloFinger &f = halo_finger;
  halo_list_update(c.list, dt);
  if (f.pressed) {  // which row the finger came down on (rows are the list's items 1..); not a banner's (ancs)
    const int i = f.owner ? -1 : halo_list_hit(c.list, f.y0) - 1;
    c.armed = i >= 0 && i < c.n && fabsf(f.x0 - CX) < M_ITEM_W / 2 * c.list.fx[i + 1].s ? i : -1;
  }
  if (f.dragging) c.armed = -1;
  if (f.tap && c.armed >= 0) {
    const CardRow &r = c.rows[c.armed];
    if (r.tap_i) r.tap_i(c.armed);
    else if (r.tap) r.tap();
  }
  for (int i = 0; i < c.n; i++) press_update(c.press[i], i == c.armed && f.down, dt);
  if (!f.down) c.armed = -1;
}

static void cl_layout(CardList &c, const HaloView &v) {
  const HaloListFx &h = c.list.fx[0];
  if (c.list.moving && v.a > 0 && v.s >= 0.999f) halo_subpixel = true;
  text_mid(c.head_it, t_title, c.head, c.mid_head, CX, h.top + M_HEAD_H / 2, 1, TEXT, h.a_bg, &halo_black);
  c.head_it.on_black = true;
  halo_view_commit(c.head_it, v);
  for (int k = 0; k < c.n; k++) {  // a card as in the gallery: a colored circle with the icon, the title, the subtitle
    const HaloListFx &f = c.list.fx[k + 1];
    const CardRow &d = c.rows[k];
    const bool vis = halo_list_item_visible(c.list, k + 1);
    const float s = f.s, hh = M_ITEM_H * s, w = M_ITEM_W * s, x0 = CX - w / 2, y0 = f.top;
    const float p = halo_clamp01(c.press[k].s.x), ac = vis ? f.a_c : 0, ix = x0 + 56 * s, iy = y0 + hh / 2;
    const float tx = x0 + (d.icon ? 104 : 56) * s;
    HaloItem &bg = c.bg[k];  // pressed — the corners squeeze (26 dp -> 18 dp)
    bg.kind = HI_RRECT, bg.x = CX, bg.y = y0 + hh / 2, bg.w = M_ITEM_W, bg.h = M_ITEM_H, bg.scale = s;
    bg.r = 52 + (36 - 52) * p, bg.color = CARD_HI, bg.alpha = vis ? f.a_bg : 0, bg.on_black = true;
    HaloItem &dc = c.disc[k];
    dc.kind = HI_CIRCLE, dc.x = ix, dc.y = iy, dc.r = 32, dc.scale = s, dc.color = d.color, dc.alpha = d.icon ? ac : 0;
    dc.on_black = true;
    const bool wifi = d.icon == CL_WIFI, bt = d.icon == CL_BT;
    text_mid(c.icon[k], ic24, d.icon && !wifi && !bt ? d.icon : "", cl_mid_icon, ix, iy, s, 0x1B1B1F, d.icon ? ac : 0, &dc);
    for (int i = 0; i < 5; i++) {
      HaloItem &w = c.wifi[k][i];
      w.color = 0x1B1B1F, w.on_black = false, w.alpha = 0;
      if (wifi && i < 4) {  // a dot and three arcs of ±45° around it
        w.kind = i ? HI_ARC : HI_CIRCLE, w.x = ix, w.y = iy + 9 * s, w.scale = s;
        w.r = i ? 7.0f * i : 3.5f, w.w = 4, w.a0 = -HALO_PI / 4, w.a1 = HALO_PI / 4, w.alpha = ac;
      } else if (bt) {  // the rune: the stem and the two loops' four strokes
        static const float P[5][4] = {{0, -13, 0, 13}, {0, -13, 7, -6}, {7, -6, -7, 7}, {0, 13, 7, 6}, {7, 6, -7, -7}};
        w.kind = HI_LINE, w.scale = 1, w.r = 1.9f * s, w.alpha = ac;
        w.x = ix + P[i][0] * s, w.y = iy + P[i][1] * s, w.w = ix + P[i][2] * s, w.h = iy + P[i][3] * s;
      }
      halo_view_commit(w, v);
    }
    const char *title = d.title ? d.title : d.title_i ? d.title_i(k) : "", *sub = d.sub ? d.sub() : d.sub_i ? d.sub_i(k) : "";
    text_base(c.title[k], t24, title, tx, y0 + 46 * s, s, TEXT, ac, &bg);
    text_base(c.sub[k], t20, sub, tx, y0 + 80 * s, s, d.icon ? DIM : d.color, ac, &bg);
    halo_view_commit(bg, v), halo_view_commit(dc, v), halo_view_commit(c.icon[k], v);
    halo_view_commit(c.title[k], v), halo_view_commit(c.sub[k], v);
  }
}

static void cl_draw(CardList &c) {
  halo_item_draw(c.head_it);
  for (int k = 0; k < c.n; k++) {
    halo_item_draw(c.bg[k]), halo_item_draw(c.disc[k]), halo_item_draw(c.icon[k]);
    halo_item_draw(c.title[k]), halo_item_draw(c.sub[k]);
    for (auto &w : c.wifi[k]) halo_item_draw(w);
  }
}

static CardList &cl_new(const char *head, const CardRow *rows, int n) {  // in PSRAM (Oriole.ino: psram_new)
  CardList &c = psram_new<CardList>();
  c.head = head, c.rows = rows, c.n = n;
  return c;
}

// A screen of a CardList (HaloScreen takes plain functions)
#define CL_SCREEN(name, list)                                                                                    \
  static const HaloScreen name = {[] { cl_enter(list); }, [](float dt) { cl_update(list, dt); },                   \
                                  [](const HaloView &v) { cl_layout(list, v); }, [] { cl_draw(list); }, false, \
                                  HALO_HOME_BOTH}

// ---------- Watch face: a tap steps the value (face.h shows it at once; in flash 0.6 s later) ----------
#define SE_VALUE 0xA8C7FA  // a row's value, in place of a subtitle
static const CardRow WF_ROWS[] = {
    {"Style", nullptr, SE_VALUE, [] { return FACE_STYLE[face_style()]; },
     [] { cfg_ui_set("face_style", (face_style() + 1) % FS_N); }},
    {"Clock color", nullptr, SE_VALUE, [] { return FACE_COLOR_NAME[constrain(cfg.face_color, 0, 4)]; },
     [] { cfg_ui_set("face_color", (cfg.face_color + 1) % 5); }},
    {"Clock", nullptr, SE_VALUE, [] { return cfg.clock24 ? "24-hour" : "12-hour"; },
     [] { cfg_ui_set("clock24", !cfg.clock24); }},
};
static CardList &wf_list = cl_new("Watch face", WF_ROWS, sizeof(WF_ROWS) / sizeof(WF_ROWS[0]));
CL_SCREEN(WATCHFACE_SCREEN, wf_list);

// ---------- Device: restart and turn off — the first tap asks, a second within 3 s does it (from loop()) ----------
enum : uint8_t { DV_NONE, DV_RESTART, DV_OFF, DV_FORGET };
static uint8_t dv_asked;           // the row tapped once, waiting for the second tap
static uint32_t dv_asked_at;
static volatile uint8_t dv_do;     // for loop(): restart or turn off
static void dv_tap(uint8_t what) {
  if (dv_asked == what && millis() - dv_asked_at < 3000) dv_do = what;
  else dv_asked = what, dv_asked_at = millis();
}
static const char *dv_sub(uint8_t what, const char *idle, const char *ask) {
  if (dv_do == what) return what == DV_OFF ? "Turning off..." : "Restarting...";
  return dv_asked == what && millis() - dv_asked_at < 3000 ? ask : idle;
}
static const CardRow DV_ROWS[] = {
    {"Restart", nullptr, SE_VALUE, [] { return dv_sub(DV_RESTART, "start again", "Tap again to restart"); },
     [] { dv_tap(DV_RESTART); }},
    {"Turn off", nullptr, 0xF2B8B5, [] { return dv_sub(DV_OFF, "PWR turns it back on", "Tap again to turn off"); },
     [] { dv_tap(DV_OFF); }},
};
static CardList &dv_list = cl_new("Device", DV_ROWS, sizeof(DV_ROWS) / sizeof(DV_ROWS[0]));
CL_SCREEN(DEVICE_SCREEN, dv_list);

// ---------- Wi-Fi: what is set and how it is (read once a second); a tap on Status connects now ----------
static char wi_ssid[36], wi_status[40], wi_signal[32], wi_ip[20], wi_idle[24];
static void wi_refresh() {
  static uint32_t at;
  if (at && millis() - at < 1000) return;
  at = millis() | 1;
  {
    CfgLock l;
    strlcpy(wi_ssid, *cfg.ssid ? cfg.ssid : "Not set", sizeof(wi_ssid));
    snprintf(wi_idle, sizeof(wi_idle), "%d s without use", cfg.net_idle);
  }
  const uint8_t st = net_state;
  static const char *const ST[] = {"Off (tap to connect)", "Connecting...", "Connected", "Failed (tap to retry)",
                                   "Set up in the web controller"};
  strlcpy(wi_status, ST[st], sizeof(wi_status));
  if (st == NET_UP) {
    const int r = WiFi.RSSI();
    snprintf(wi_signal, sizeof(wi_signal), "%s, %d dBm",
             r >= -55 ? "Excellent" : r >= -67 ? "Good" : r >= -75 ? "Fair" : r >= -85 ? "Weak" : "Very weak", r);
    strlcpy(wi_ip, WiFi.localIP().toString().c_str(), sizeof(wi_ip));
  } else {
    strcpy(wi_signal, "-"), strcpy(wi_ip, "-");
  }
}
static const CardRow WI_ROWS[] = {
    {"Network", nullptr, SE_VALUE, [] { return wi_refresh(), (const char *)wi_ssid; }, [] {}},
    {"Status", nullptr, SE_VALUE, [] { return wi_refresh(), (const char *)wi_status; },
     [] {
       if (net_state != NET_UP && net_state != NET_NO_SSID) net_poke();
     }},
    {"Signal", nullptr, SE_VALUE, [] { return wi_refresh(), (const char *)wi_signal; }, [] {}},
    {"IP address", nullptr, SE_VALUE, [] { return wi_refresh(), (const char *)wi_ip; }, [] {}},
    {"Turns off after", nullptr, SE_VALUE, [] { return wi_refresh(), (const char *)wi_idle; }, [] {}},
};
static CardList &wi_list = cl_new("Wi-Fi", WI_ROWS, sizeof(WI_ROWS) / sizeof(WI_ROWS[0]));
CL_SCREEN(WIFI_SCREEN, wi_list);

// ---------- Assistant: Gemini's voice, the language of the replies, Google Search (in flash; the next session uses them) ----------
static const char *const AS_VOICE[] = {"Puck", "Charon", "Kore", "Fenrir", "Aoede", "Leda", "Orus", "Zephyr"};
static const char *const AS_LANG[][2] = {{"", "Auto"},          {"en-US", "English"},  {"es-US", "Spanish"},
                                         {"fr-FR", "French"},   {"de-DE", "German"},   {"it-IT", "Italian"},
                                         {"pt-BR", "Portuguese"}, {"ja-JP", "Japanese"}, {"ko-KR", "Korean"},
                                         {"cmn-CN", "Chinese"}, {"hi-IN", "Hindi"},    {"tr-TR", "Turkish"},
                                         {"ru-RU", "Russian"}};
#define AS_NV (int)(sizeof(AS_VOICE) / sizeof(AS_VOICE[0]))
#define AS_NL (int)(sizeof(AS_LANG) / sizeof(AS_LANG[0]))
static int as_voice_i() {  // the setting's index; another name (set from the controller) — −1
  for (int i = 0; i < AS_NV; i++)
    if (!strcasecmp(cfg.voice, AS_VOICE[i])) return i;
  return -1;
}
static int as_lang_i() {
  for (int i = 0; i < AS_NL; i++)
    if (!strcasecmp(cfg.lang, AS_LANG[i][0])) return i;
  return -1;
}
static const CardRow AS_ROWS[] = {
    {"Voice", nullptr, SE_VALUE, [] { return as_voice_i() >= 0 ? AS_VOICE[as_voice_i()] : (const char *)cfg.voice; },
     [] { cfg_ui_set_str("ai_voice", AS_VOICE[(as_voice_i() + 1) % AS_NV]); }},
    {"Language", nullptr, SE_VALUE, [] { return as_lang_i() >= 0 ? AS_LANG[as_lang_i()][1] : (const char *)cfg.lang; },
     [] { cfg_ui_set_str("ai_lang", AS_LANG[(as_lang_i() + 1) % AS_NL][0]); }},
    {"Google Search", nullptr, SE_VALUE, [] { return cfg.search ? "On: answers can use the web" : "Off"; },
     [] { cfg_ui_set("ai_search", !cfg.search); }},
};
static CardList &asst_list = cl_new("Assistant", AS_ROWS, sizeof(AS_ROWS) / sizeof(AS_ROWS[0]));
CL_SCREEN(ASSISTANT_SET_SCREEN, asst_list);

// ---------- Bluetooth: iPhone notifications (ancs.h) ----------
static const CardRow BT_ROWS[] = {
    {"Status", nullptr, SE_VALUE,
     [] {
       static const char *const S[] = {"Off", "Waiting for the iPhone", "Connected, pairing...", "Paired, setting up...",
                                       "Getting notifications", "Connected, but no notifications",
                                       "Paused while Wi-Fi is on"};
       return S[bt_state];
     },
     [] {}},
    {"Bluetooth", nullptr, SE_VALUE, [] { return cfg.bt_on ? "On (the watch sleeps less)" : "Off"; },
     [] { cfg_ui_set("bt_on", !cfg.bt_on); }},
    {"Forget iPhone", nullptr, 0xF2B8B5, [] { return dv_sub(DV_FORGET, "then forget Oriole on the iPhone", "Tap again to forget"); },
     [] { dv_tap(DV_FORGET); }},
};
static CardList &bt_list = cl_new("Bluetooth", BT_ROWS, sizeof(BT_ROWS) / sizeof(BT_ROWS[0]));
CL_SCREEN(BT_SCREEN, bt_list);
static const char BT_HOW[] = "On the iPhone: Settings, Bluetooth, Oriole";

// ---------- Settings ----------
static const CardRow SE_ROWS[] = {
    {"Watch face", "", 0xA8C7FA, [] { return "style, color, 12/24 h"; }, [] { halo_nav_open(WATCHFACE_SCREEN); }},
    {"Display", "", 0xFDD663, [] { return "brightness"; }, [] { halo_nav_open(DISPLAY_SCREEN); }},
    {"Sound", "", 0xF6AEA9, [] { return "volume, microphone"; }, [] { halo_nav_open(SOUND_SCREEN); }},
    {"Assistant", "\uf130", 0x8AB4F8, [] { return "voice, language"; }, [] { halo_nav_open(ASSISTANT_SET_SCREEN); }},
    {"Bluetooth", CL_BT, 0x8AB4F8, [] { return bt_state == BT_READY ? "iPhone connected" : "iPhone notifications"; },
     [] { halo_nav_open(BT_SCREEN); }},
    {"Wi-Fi", CL_WIFI, 0x7FCFFF, [] { return "network and status"; }, [] { halo_nav_open(WIFI_SCREEN); }},
    {"Battery", "", 0x5CF08A, [] { return "level and history"; }, [] { halo_nav_open(BATTERY_SCREEN); }},
    {"Device", "", 0xC9B6FF, [] { return "restart, turn off"; }, [] { halo_nav_open(DEVICE_SCREEN); }},
};
static CardList &se_list = cl_new("Settings", SE_ROWS, sizeof(SE_ROWS) / sizeof(SE_ROWS[0]));

static void settings_build() {
  cl_build(se_list), cl_build(wf_list), cl_build(dv_list), cl_build(wi_list), cl_build(asst_list), cl_build(bt_list);
}

// The device's restart or power-off, from loop(): first everything still waiting is saved
static void device_poll() {
  if (dv_do == DV_FORGET) return dv_do = DV_NONE, dv_asked = DV_NONE, bt_forget();
  if (!dv_do) return;
  settings_save_poll(true), cfg_ui_poll(true), steps_poll(true);
  LOG("device: %s\n", dv_do == DV_OFF ? "turning off" : "restarting");
  delay(300);  // the last frame ("Restarting...") shown, the log out
  if (dv_do == DV_RESTART) esp_restart();
  halo_display_on(false);
  delay(100);
  uint8_t r = 0;  // AXP2101, register 0x10 (common configuration), bit 0: soft power-off
  HaloI2c pmu = halo_i2c(0x34);
  if (pmu && halo_i2c_read(pmu, 0x10, &r, 1)) halo_i2c_write(pmu, 0x10, r | 1);
  delay(1000);
  LOG("device: still on (the power chip did not answer)\n");
  dv_do = DV_NONE, halo_display_on(true);
}

// To the face — both ways, as on the gallery list: down from the top edge or up from the bottom edge (only from the very
// edge: the rest of the screen scrolls the list)
CL_SCREEN(SETTINGS, se_list);
