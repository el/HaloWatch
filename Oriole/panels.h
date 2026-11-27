// panels.h — beside the watch face, as on Pixel Watch:
//   Quick settings — swipe right on the face (it comes from the left): the batteries, six buttons (Do Not Disturb,
//                    mute, night mode, brightness low/high, Bluetooth, lock the screen) — a long press opens its
//                    page. Swipe left or up to close it
//   Tiles          — swipe left on the face: Weather, Next event, Now playing (play/pause), Timer; sideways
//                    between them, swipe right on the first (or up from the bottom edge) to go back
//   Notifications  — swipe down from the face's top edge (apps.h: NOTIF_FACE_SCREEN)
// Both open right over the face (oriole_nav_face_open). Included from Oriole.ino after home.h.

static uint8_t pn_token;  // the face's swipe that opens a panel is the face's: not the panel's pager or slider

// ---------- Quick settings ----------
// Six buttons; a tap toggles, a long press opens its page (qs_goto: Oriole.ino closes this, opens Settings and the page)
enum { QS_DND, QS_MUTE, QS_NIGHT, QS_BRIGHT, QS_BT, QS_LOCK, QS_N };
static const float QS_X[QS_N] = {CX - 100, CX, CX + 100, CX - 100, CX, CX + 100};
static const float QS_Y[QS_N] = {204, 204, 204, 300, 300, 300};
#define QS_R 40.0f
#define QS_CHIP_Y 108.0f
#define QS_ON 0xA8C7FA
#define QS_ON_INK 0x0B2A5C
#define QS_LOW 2   // brightness steps (display.h): Low ...
#define QS_HIGH 7  // ... and High (the default)
struct QsUi {
  Press press[QS_N];
  OrioleItem top, disc[QS_N], ic[QS_N], bt[5];
  OrioleItem chip[2], chip_t[2], bolt;  // the batteries: the watch's and the iPhone's
  char top_s[48], chip_s[2][24];
};
static QsUi &qs = psram_new<QsUi>();
static int qs_armed = -1, qs_prev_vol = 60;
static bool qs_held;  // this press was a long one: no tap when it ends
static uint32_t qs_note_until;
static float qs_mid_top, qs_mid_ic, qs_mid_bolt;
// A long press: the app (APP_SCREENS index, −1 — Settings) and the Settings page to open; Oriole.ino takes it
static volatile int qs_goto_app = -2;
static const OrioleScreen *qs_goto_page;

static bool qs_on(int i) {
  return i == QS_DND ? dnd_on : i == QS_MUTE ? cfg.vol == 0 : i == QS_NIGHT ? cfg.night != 0
       : i == QS_BRIGHT ? dp_sl.value >= QS_HIGH - 1 : i == QS_BT ? cfg.bt_on : false;
}
static void qs_note(const char *s) {  // in place of the battery chips for a moment: what the tap did
  strlcpy(qs.top_s, s, sizeof(qs.top_s)), qs_note_until = millis() + 1500;
}

static void qs_tap(int i) {
  switch (i) {
    case QS_DND:
      dnd_on = !dnd_on, qs_note(dnd_on ? "Do Not Disturb on" : "Do Not Disturb off");
      break;
    case QS_MUTE: {
      if (cfg.vol) qs_prev_vol = cfg.vol;
      const int v = cfg.vol ? 0 : qs_prev_vol;
      oriole_audio_volume(v), cfg_ui_set("volume", v), qs_note(v ? "Sound on" : "Muted");
      break;
    }
    case QS_NIGHT:
      cfg_ui_set("night", cfg.night ? 0 : 3), fc_check = true, qs_note(cfg.night ? "Night mode on" : "Night mode off");
      break;
    case QS_BRIGHT: {
      const bool high = !qs_on(QS_BRIGHT);
      slider_set(dp_sl, high ? QS_HIGH : QS_LOW), qs_note(high ? "Brightness high" : "Brightness low");
      break;
    }
    case QS_BT:
      cfg_ui_set("bt_on", !cfg.bt_on), qs_note(cfg.bt_on ? "Bluetooth on" : "Bluetooth off");
      break;
    case QS_LOCK:  // closes, then locks (a long press on PWR unlocks)
      oriole_nav_back(), dp_lock_set(true);
      break;
  }
}

static void qs_long(int i) {  // its page: Do Not Disturb — the notifications, the rest — their Settings page
  static const OrioleScreen *const PAGE[QS_N] = {nullptr,  &SOUND_SCREEN, &WATCHFACE_SCREEN,
                                               &DISPLAY_SCREEN, &BT_SCREEN, &DISPLAY_SCREEN};
  qs_goto_page = PAGE[i], qs_goto_app = i == QS_DND ? 1 : -1;
  oriole_finger_claim(&qs);  // still held: not the face's long press (its style picker) once this closes
  oriole_nav_back();
}

static void qs_enter() {
  qs_armed = -1, qs_held = false, qs_note_until = 0;
  for (auto &p : qs.press) oriole_spring_set(p.s, 0), p.going_down = false;
}

static void qs_update(float dt) {
  OrioleFinger &f = oriole_finger;
  if (f.drag_start && (f.horiz ? f.x < f.x0 : f.y < f.y0) && oriole_finger_claim(&qs)) return oriole_nav_back();  // close
  if (f.pressed) {
    qs_armed = -1, qs_held = false;
    for (int i = 0; i < QS_N && !f.owner; i++)
      if (hypotf(f.x0 - QS_X[i], f.y0 - QS_Y[i]) < QS_R + 8) qs_armed = i;
  }
  if (f.dragging) qs_armed = -1;
  if (qs_armed >= 0 && f.down && !qs_held && oriole_ms() - f.t0 > 500) qs_held = true, qs_long(qs_armed);
  if (f.tap && qs_armed >= 0 && !qs_held) qs_tap(qs_armed);
  for (int i = 0; i < QS_N; i++) press_update(qs.press[i], i == qs_armed && f.down && !qs_held, dt);
  if (!f.down) qs_armed = -1;
}

static void qs_layout(const OrioleView &v) {
  // Two chips on top (inside the circle at this height), or for a moment what a tap did
  const bool note = (int32_t)(millis() - qs_note_until) < 0;
  text_mid(qs.top, t24, qs.top_s, qs_mid_top, CX, QS_CHIP_Y, 1, TEXT, note ? 1 : 0, &oriole_black);
  qs.top.on_black = true;
  oriole_view_commit(qs.top, v);
  const OrioleBattery b = oriole_battery();
  const int pct[2] = {b.ok ? b.percent : -1, face_iphone_batt()};
  const bool charging = b.ok && b.charging;
  for (int i = 0; i < 2; i++) {
    const float x = CX + (i ? 82 : -82), ca = note ? 0 : 1;
    OrioleItem &c = qs.chip[i];
    c.kind = HI_RRECT, c.x = x, c.y = QS_CHIP_Y, c.w = 156, c.h = 44, c.r = 22, c.scale = 1, c.color = CARD_HI;
    c.alpha = ca, c.on_black = true;
    oriole_view_commit(c, v);
    if (pct[i] >= 0) snprintf(qs.chip_s[i], sizeof(qs.chip_s[i]), "%s %d%%", i ? "iPhone" : "Watch", pct[i]);
    else snprintf(qs.chip_s[i], sizeof(qs.chip_s[i]), "%s --", i ? "iPhone" : "Watch");
    const uint32_t col = pct[i] >= 0 && pct[i] < 20 ? 0xF28B82 : !i && charging ? 0x5CF08A : TEXT;
    const bool bolt = !i && charging;
    text_mid(qs.chip_t[i], t20, qs.chip_s[i], ap_mid_t20, x + (bolt ? 11 : 0), QS_CHIP_Y, 1, col, ca, &c);
    oriole_view_commit(qs.chip_t[i], v);
  }
  text_mid(qs.bolt, ic18, "", qs_mid_bolt, CX - 82 - 60, QS_CHIP_Y, 1, 0x5CF08A, charging && !note ? 1 : 0,
           &qs.chip[0]);
  oriole_view_commit(qs.bolt, v);
  // Bell-slash, speaker, moon, sun, (Bluetooth: drawn below), lock
  static const char *const IC[QS_N] = {"", "", "", "", "", ""};
  for (int i = 0; i < QS_N; i++) {
    const bool on = qs_on(i);
    const float s = 1 - 0.1f * oriole_clamp01(qs.press[i].s.x);
    OrioleItem &d = qs.disc[i];
    d.kind = HI_CIRCLE, d.x = QS_X[i], d.y = QS_Y[i], d.r = QS_R, d.scale = s, d.color = on ? QS_ON : CARD_HI;
    d.alpha = 1, d.on_black = true;
    oriole_view_commit(d, v);
    const char *g = i == QS_MUTE && on ? "" : IC[i];
    text_mid(qs.ic[i], ic24, g, qs_mid_ic, QS_X[i], QS_Y[i], s, on ? QS_ON_INK : TEXT, 1, &d);
    oriole_view_commit(qs.ic[i], v);
  }
  // Bluetooth: the rune drawn with lines (the icon font has none), as in Settings
  static const float P[5][4] = {{0, -13, 0, 13}, {0, -13, 7, -6}, {7, -6, -7, 7}, {0, 13, 7, 6}, {7, 6, -7, -7}};
  const float bx = QS_X[QS_BT] - 2, by = QS_Y[QS_BT], bs = 1 - 0.1f * oriole_clamp01(qs.press[QS_BT].s.x);
  for (int i = 0; i < 5; i++) {
    OrioleItem &w = qs.bt[i];
    w.kind = HI_LINE, w.scale = 1, w.r = 1.9f * bs, w.alpha = 1, w.on_black = false;
    w.color = qs_on(QS_BT) ? QS_ON_INK : TEXT;
    w.x = bx + P[i][0] * bs, w.y = by + P[i][1] * bs, w.w = bx + P[i][2] * bs, w.h = by + P[i][3] * bs;
    oriole_view_commit(w, v);
  }
}

static void qs_draw() {
  oriole_item_draw(qs.top);
  for (int i = 0; i < 2; i++) oriole_item_draw(qs.chip[i]), oriole_item_draw(qs.chip_t[i]);
  oriole_item_draw(qs.bolt);
  for (int i = 0; i < QS_N; i++) oriole_item_draw(qs.disc[i]), oriole_item_draw(qs.ic[i]);
  for (auto &w : qs.bt) oriole_item_draw(w);
}

static const OrioleScreen QS_SCREEN = {qs_enter, qs_update, qs_layout, qs_draw};

// ---------- Tiles ----------
enum { TL_WEATHER, TL_EVENT, TL_MUSIC, TL_TIMER, TL_N };
#define TL_BTN_Y 322.0f
struct Tile {
  OrioleItem head, big, l1, l2, ring, disc, btn[7];
  char head_s[40], big_s[64], l1_s[64], l2_s[64];
  const OrioleFont *bf;  // the big line's font
  float ring_v;        // −1 — no ring
  uint32_t ring_c;
  bool button, playing;
};
struct TilesUi {
  Tile t[TL_N];
  OriolePager pg;
};
static TilesUi &tl = psram_new<TilesUi>();
static uint32_t tl_filled;
static float tl_mid_n48, tl_mid_title, tl_mid_t24, tl_mid_t20;

static void tl_fill() {  // the tiles' text, twice a second
  tl_filled = millis();
  for (auto &t : tl.t) t.ring_v = -1, t.button = false, t.l2_s[0] = 0, t.bf = &t_title;
  {  // Weather: the first city
    Tile &t = tl.t[TL_WEATHER];
    const WxDay &d = wx_days[0];
    strlcpy(t.head_s, wx_city_name[0], sizeof(t.head_s));
    t.bf = &n48;
    if (wx_ok[0]) {
      snprintf(t.big_s, sizeof(t.big_s), "%d°", d.now);
      strlcpy(t.l1_s, WX_WORD[d.kind & 3], sizeof(t.l1_s));
      snprintf(t.l2_s, sizeof(t.l2_s), "High %d°  Low %d°", d.hi, d.lo);
    } else strcpy(t.big_s, "--"), strcpy(t.l1_s, "No weather yet");
  }
  {  // Next event: the Calendar screen's first row (apps.h)
    Tile &t = tl.t[TL_EVENT];
    if (cal_ver != ca_seen) ca_fill();
    strcpy(t.head_s, ca_n ? "Next up" : "Calendar");
    fit_text(t.big_s, sizeof(t.big_s), t_title, ca_t[0], 330);
    fit_text(t.l1_s, sizeof(t.l1_s), t24, ca_s[0], 340);
  }
  {  // Now playing
    Tile &t = tl.t[TL_MUSIC];
    static Media &m = psram_new<Media>();
    portENTER_CRITICAL(&md_mux);
    m = md;
    portEXIT_CRITICAL(&md_mux);
    if (m.valid && m.title[0]) {
      fit_text(t.head_s, sizeof(t.head_s), t20, m.player[0] ? m.player : "Music", 260);
      fit_text(t.big_s, sizeof(t.big_s), t_title, m.title, 330);
      fit_text(t.l1_s, sizeof(t.l1_s), t24, m.artist, 330);
      t.button = true, t.playing = m.state == 1;
    } else {
      strcpy(t.head_s, "Music"), strcpy(t.big_s, "Nothing playing");
      strcpy(t.l1_s, m.valid ? "Play something on the iPhone" : "Connect your iPhone");
    }
  }
  {  // Timer
    Tile &t = tl.t[TL_TIMER];
    strcpy(t.head_s, "Timer");
    t.bf = &n48;
    if (tm_state == TM_RUN || tm_state == TM_PAUSE) {
      const int s = (int)((timer_ms_left() + 999) / 1000);
      snprintf(t.big_s, sizeof(t.big_s), "%d:%02d", s / 60, s % 60);
      strcpy(t.l1_s, tm_state == TM_RUN ? "Running" : "Paused");
      t.ring_v = timer_face_fraction(), t.ring_c = 0xFFC56B;
    } else t.bf = &t_title, strcpy(t.big_s, tm_state == TM_DONE ? "Done" : "Not running"), strcpy(t.l1_s, "Start one in Timer");
  }
}

static void tiles_enter() {
  oriole_pager_begin(tl.pg, TL_N);
  tl_fill();
}

static void tiles_update(float dt) {
  OrioleFinger &f = oriole_finger;
  if (tl.pg.page == 0 && fabsf(tl.pg.off.x) < 0.01f && f.drag_start && f.horiz && f.x > f.x0 && oriole_finger_claim(&tl))
    return oriole_nav_back();  // right on the first tile: back to the face
  oriole_pager_update(tl.pg, dt);
  const Tile &m = tl.t[TL_MUSIC];
  if (f.tap && !f.owner && tl.pg.page == TL_MUSIC && m.button && hypotf(f.x - CX, f.y - TL_BTN_Y) < 56)
    ams_cmd(AMS_TOGGLE), tl.t[TL_MUSIC].playing = !m.playing;
  if (millis() - tl_filled > 500) tl_fill();
}

static void tiles_layout(const OrioleView &v) {
  for (int i = 0; i < TL_N; i++) {
    Tile &t = tl.t[i];
    OrioleView vt = v;
    vt.x += oriole_pager_x(tl.pg, i) * v.s;
    const float a = oriole_pager_visible(tl.pg, i) ? 1 : 0;
    const bool ring = t.ring_v >= 0;
    t.ring.kind = HI_RING, t.ring.x = CX, t.ring.y = CX, t.ring.r = 214, t.ring.w = 12, t.ring.scale = 1;
    t.ring.v = fmaxf(0, t.ring_v), t.ring.color = t.ring_c, t.ring.color2 = 0x2A2C30, t.ring.alpha = ring ? a : 0;
    t.ring.on_black = true;
    oriole_view_commit(t.ring, vt);
    const float big_y = t.bf == &n48 ? 196 : 196;
    text_mid(t.head, t20, t.head_s, tl_mid_t20, CX, 112, 1, DIM, a, &oriole_black);
    text_mid(t.big, *t.bf, t.big_s, t.bf == &n48 ? tl_mid_n48 : tl_mid_title, CX, big_y, t.bf == &n48 ? 1.6f : 1, TEXT,
             a, &oriole_black);
    text_mid(t.l1, t24, t.l1_s, tl_mid_t24, CX, 262, 1, i == TL_WEATHER ? TEXT : DIM, a, &oriole_black);
    text_mid(t.l2, t20, t.l2_s, tl_mid_t20, CX, 300, 1, DIM, a, &oriole_black);
    for (OrioleItem *it : {&t.head, &t.big, &t.l1, &t.l2}) it->on_black = true, oriole_view_commit(*it, vt);
    // Now playing: play / pause
    const float ba = t.button ? a : 0;
    t.disc.kind = HI_CIRCLE, t.disc.x = CX, t.disc.y = TL_BTN_Y, t.disc.r = 46, t.disc.scale = 1, t.disc.color = QS_ON;
    t.disc.alpha = ba, t.disc.on_black = true;
    oriole_view_commit(t.disc, vt);
    if (t.playing) {  // two bars
      for (int k = 0; k < 2; k++) {
        OrioleItem &l = t.btn[k];
        l.kind = HI_LINE, l.x = l.w = CX + (k ? 8 : -8), l.y = TL_BTN_Y - 13, l.h = TL_BTN_Y + 13, l.r = 4, l.scale = 1;
        l.color = QS_ON_INK, l.alpha = ba, l.on_black = false;
        oriole_view_commit(l, vt);
      }
      mu_hide(t.btn, 2, 7, vt);
    } else mu_hide(t.btn, mu_tri(t.btn, CX + 3, TL_BTN_Y, 30, 1, QS_ON_INK, ba, vt), 7, vt);
  }
  for (int i = 0; i < tl.pg.count; i++) {  // the dots, faded with the screen
    OrioleItem &d = tl.pg.dots[i];
    d.scale = 1, d.alpha = 1, d.on_black = true;
    oriole_view_commit(d, v);
  }
}

static void tiles_draw() {
  for (Tile &t : tl.t) {
    oriole_item_draw(t.ring), oriole_item_draw(t.head), oriole_item_draw(t.big), oriole_item_draw(t.l1), oriole_item_draw(t.l2);
    oriole_item_draw(t.disc);
    for (auto &b : t.btn) oriole_item_draw(b);
  }
  oriole_pager_draw(tl.pg);
}

static const OrioleScreen TILES_SCREEN = {tiles_enter, tiles_update, tiles_layout, tiles_draw};

// ---------- From the face (face.h) ----------
static bool face_panel_swipe(OrioleFinger &f) {  // a swipe that opens a panel: true — taken
  if (!f.drag_start || f.owner) return false;
  if (f.horiz && f.x < f.x0) {  // left: the tiles come in from the right edge
    oriole_finger_claim(&pn_token);
    return oriole_nav_face_open(TILES_SCREEN, ORIOLE_FX_ZOOM, W - 40, CX, 80, 240), true;
  }
  if (f.horiz) {  // right: Quick settings from the left edge
    oriole_finger_claim(&pn_token);
    return oriole_nav_face_open(QS_SCREEN, ORIOLE_FX_ZOOM, 40, CX, 80, 240), true;
  }
  if (f.y > f.y0 && f.y0 < 100) {  // down from the top edge: the notifications
    oriole_finger_claim(&pn_token);
    return oriole_nav_face_open(NOTIF_FACE_SCREEN, ORIOLE_FX_ZOOM, CX, 40, 240, 80), true;
  }
  return false;
}

static void panels_build() {
  qs_mid_top = oriole_text_mid(t24, "Battery 0123456789%");
  qs_mid_bolt = oriole_text_mid(ic18, "\uf0e7");
  qs_mid_ic = oriole_text_mid(ic24, "");
  tl_mid_n48 = oriole_text_mid(n48, "0123456789");
  tl_mid_title = oriole_text_mid(t_title, "Hello");
  tl_mid_t24 = oriole_text_mid(t24, "Hello");
  tl_mid_t20 = oriole_text_mid(t20, "Hello");
  oriole_pager_begin(tl.pg, TL_N);
}
