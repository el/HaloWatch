// display.h — the "Display" screen (Settings -> Display): the screen's brightness, and the PWR lock.
//
// The slider is WearKit's (demo_slider.h: Wear OS 6 InlineSlider, numbers — docs/research/notes/pickers_progress.md)
// ported to Oriole: a 52 dp pill with "−" and "+" at its ends (zones of 48 dp), the bar between them — the filled part
// 12 dp thick, the empty one 4 dp, with a dot between every two steps (dark on the filled part, light on the empty one).
// A tap on "−"/"+" — one step (a light circle grows under the icon while pressed), holding — a repeat after 500 ms,
// then every 60 ms; a finger on the bar sets the value by its position (not on Wear OS: watches have a crown). The bar
// catches up with a new value in 150 ms along (0, 0, 0, 1) — sharp at first, soft at the end — and the brightness of
// the screen glides along with it. The level is kept in flash (settings.h) and applied at start.
//
// Going idle (power.h): the brightness fades to zero in about 0.25 s here (dp_power); Always On (power.h) then sets
// the panel's brightness itself (pw_bright) and keeps the panel lit at zero (pw_keep_lit). BOOT is the assistant's
// push-to-talk (assistant.h).
//
// The lock (PWR, anywhere): a click locks — touches stop working (oriole_touch_on: no taps, swipes or gestures) and the
// brightness glides down to the slider's lowest step; a long press (~1.5 s, the power chip decides; holding 6–10 s
// powers the board off in hardware) unlocks — the brightness glides back to the slider's value. While locked, a small
// lock sits at the very top, where the Weather screen and the gallery show the time (that time gives way to it, ui_lock):
// it fades and grows in, wiggles when the locked screen is touched, and opens (the open lock) as it fades out on
// unlocking. The lock is kept in flash (settings.h): after a reboot the watch starts locked. Commands pwr / pwr long —
// the same presses.
// Included from Oriole.ino after face.h, before settings.h.

#define DP_STEPS 8
#define DP_DEFAULT 7  // = 180, the brightness before this screen existed
// Panel brightness (command 0x51) for each step: roughly even steps for the eye (it sees brightness about as a
// logarithm). The lowest must stay visible: at 6 the panel looked fully off (only BOOT turns the screen off)
static const uint8_t DP_LEVEL[DP_STEPS + 1] = {24, 34, 48, 66, 90, 118, 150, 180, 255};
#define DP_W 410.0f  // the pill: 205 dp x 52 dp
#define DP_H 104.0f
#define DP_BTN 96.0f  // "−" and "+" zones
#define DP_HEAD_Y 96.0f
#define DP_LABEL_Y 166.0f
#define DP_SL_Y 240.0f
#define DP_HINT_Y 334.0f
#define DP_PRIMARY 0xA8C7FA  // Wear OS 6 dark scheme
#define DP_ON_PRIMARY 0x0B2A5C
#define DP_OUTLINE 0x8E9099
#define DP_TRACK 0x45474D    // the empty bar; a pressed "−"/"+"

// ---------- The slider (also on the Sound screen, sound.h) ----------
#define SL_MAX_DOTS 15
static OrioleEase EASE_SHARP;
static float sl_mid_icon;
struct Slider {
  float y;                    // the pill's middle
  int steps;                  // the value is 0..steps
  volatile int value;         // read by loop() too (saving)
  float shown, from, t = 9;   // in steps; the 150 ms catch-up
  Press press[2];             // "−", "+"
  int armed;                  // the finger came down on: −1 — "−", 1 — "+", 2 — the bar, 0 — nothing
  uint32_t hold_at;
  OrioleItem pill, track, fill, dot[SL_MAX_DOTS], circle[2], icon[2];
};

static void slider_init(Slider &s, float y, int steps, int value) {
  s.y = y, s.steps = min(steps, SL_MAX_DOTS + 1), s.value = constrain(value, 0, s.steps), s.shown = s.from = s.value;
}

static void slider_set(Slider &s, int v) {
  v = constrain(v, 0, s.steps);
  if (v == s.value) return;
  s.from = s.shown, s.t = 0, s.value = v;
}

static void slider_tick(Slider &s, float dt) {  // the catch-up: every frame, on screen or not
  s.t = fminf(s.t + dt, 9);
  s.shown = s.from + (s.value - s.from) * oriole_ease(EASE_SHARP, s.t / 0.15f);
}

static void slider_enter(Slider &s) {
  s.armed = 0;
  for (auto &p : s.press) oriole_spring_set(p.s, 0), p.going_down = false;
}

static void slider_update(Slider &s, float dt) {
  OrioleFinger &f = oriole_finger;
  const float x0 = CX - DP_W / 2, x1 = CX + DP_W / 2, bx = x0 + DP_BTN, bw = DP_W - 2 * DP_BTN;
  const bool on_pill = fabsf(f.y0 - s.y) < DP_H / 2 && f.x0 > x0 && f.x0 < x1;
  if (f.pressed) {
    s.armed = !on_pill || f.owner ? 0 : f.x0 < bx ? -1 : f.x0 > bx + bw ? 1 : 2;
    s.hold_at = oriole_ms() + 500;
  }
  if (s.armed == 2 && f.down && (!f.dragging || f.horiz) && oriole_finger_claim(&s))  // the bar: by position
    slider_set(s, (int)lroundf(oriole_clamp01((f.x - bx) / bw) * s.steps));
  if ((s.armed == -1 || s.armed == 1) && f.dragging) s.armed = 0;  // moved — a swipe, not a press
  if ((s.armed == -1 || s.armed == 1) && f.tap) slider_set(s, s.value + s.armed);
  if ((s.armed == -1 || s.armed == 1) && f.down && (int32_t)(oriole_ms() - s.hold_at) >= 0)  // held: a repeat
    slider_set(s, s.value + s.armed), s.hold_at = oriole_ms() + 60;
  press_update(s.press[0], s.armed == -1 && f.down, dt);
  press_update(s.press[1], s.armed == 1 && f.down, dt);
  if (!f.down) s.armed = 0;
}

static void slider_layout(Slider &s, const OrioleView &v) {
  // The pill and the pressed buttons' circles under the icons
  s.pill.kind = HI_RRECT, s.pill.x = CX, s.pill.y = s.y, s.pill.w = DP_W, s.pill.h = DP_H, s.pill.r = DP_H / 2;
  s.pill.scale = 1, s.pill.alpha = 1, s.pill.color = CARD_HI, s.pill.on_black = true;
  oriole_view_commit(s.pill, v);
  for (int i = 0; i < 2; i++) {
    const float p = oriole_clamp01(s.press[i].s.x), cx = i ? CX + DP_W / 2 - DP_BTN / 2 : CX - DP_W / 2 + DP_BTN / 2;
    OrioleItem &c = s.circle[i];
    c.kind = HI_CIRCLE, c.x = cx, c.y = s.y, c.r = 40 * p, c.scale = 1, c.color = DP_TRACK, c.on_black = false;
    c.alpha = p > 0.01f ? 1 : 0;
    oriole_view_commit(c, v);
    text_mid(s.icon[i], ic32, i ? "" : "", sl_mid_icon, cx, s.y, 1, TEXT, 1, nullptr);
    oriole_view_commit(s.icon[i], v);
  }
  // The bar: the empty part thin (4 dp), the filled one thick (12 dp), round ends; dots between the steps
  const float bx = CX - DP_W / 2 + DP_BTN, bw = DP_W - 2 * DP_BTN, r = oriole_clamp01(s.shown / s.steps), filled = bw * r;
  s.track.kind = HI_RRECT, s.track.x = bx + bw / 2, s.track.y = s.y, s.track.w = bw, s.track.h = 8, s.track.r = 4;
  s.track.scale = 1, s.track.alpha = 1, s.track.color = DP_TRACK, s.track.on_black = false;
  s.fill.kind = HI_RRECT, s.fill.x = bx + filled / 2, s.fill.y = s.y, s.fill.w = filled, s.fill.h = 24, s.fill.r = 12;
  s.fill.scale = 1, s.fill.alpha = filled > 1 ? 1 : 0, s.fill.color = DP_PRIMARY, s.fill.on_black = false;
  oriole_view_commit(s.track, v), oriole_view_commit(s.fill, v);
  for (int i = 1; i < s.steps; i++) {  // at the filled part's round end — no dot: the end itself marks the step
    OrioleItem &d = s.dot[i - 1];
    d.kind = HI_CIRCLE, d.x = bx + i * (bw + 8) / s.steps - 4, d.y = s.y, d.r = 4, d.scale = 1;
    d.alpha = oriole_clamp01((fabsf(d.x - (bx + filled)) - 6) / 6);
    d.color = d.x < bx + filled ? DP_ON_PRIMARY : DP_OUTLINE, d.on_black = false;
    oriole_view_commit(d, v);
  }
}

static void slider_draw(Slider &s) {
  oriole_item_draw(s.pill);
  for (int i = 0; i < 2; i++) oriole_item_draw(s.circle[i]), oriole_item_draw(s.icon[i]);
  oriole_item_draw(s.track), oriole_item_draw(s.fill);
  for (int i = 1; i < s.steps; i++) oriole_item_draw(s.dot[i - 1]);
}

// ---------- The Display screen ----------
static Slider &dp_sl = psram_new<Slider>();  // in PSRAM; the brightness, 0..DP_STEPS; settings.h saves it (from loop())
static OrioleSpring dp_power = {1, 0, 1};  // 1 — the screen is on, 0 — off (BOOT)
static OrioleSpringK K_DP_POWER;
static int dp_sent = -1;  // the brightness last sent to the panel
static float dp_mid_head, dp_mid_label, dp_mid_hint;
static OrioleItem dp_head, dp_label, dp_hint;
// The lock
#define DP_LOCK_Y 15.0f  // the lock's middle: where the time's letters are at the top (radius 219 from the center)
static volatile bool dp_locked;  // settings.h saves it (from loop())
static OrioleSpring dp_lock;       // 0 — unlocked, 1 — locked: the brightness and the lock icon glide between the two
static OrioleSpringK K_DP_LOCK;
static uint32_t dp_shake_at;     // the locked screen was touched then: the lock wiggles
static float dp_mid_lock;
static OrioleItem dp_lock_ic;

static uint8_t dp_level(float steps) {  // between the table's steps — linearly
  const float s = oriole_clamp01(steps / DP_STEPS) * DP_STEPS;
  const int i = min((int)s, DP_STEPS - 1);
  return (uint8_t)lroundf(DP_LEVEL[i] + (DP_LEVEL[i + 1] - DP_LEVEL[i]) * (s - i));
}

static uint8_t dp_start_level() {  // the first frame's brightness: the slider's, or the lowest when locked
  return dp_locked ? DP_LEVEL[0] : DP_LEVEL[dp_sl.value];
}

static void display_build() {
  dp_mid_head = oriole_text_mid(t_title, "Display");
  dp_mid_label = oriole_text_mid(t20, "Brightness");
  dp_mid_hint = oriole_text_mid(t20, "Press PWR to lock the screen");
  sl_mid_icon = oriole_text_mid(ic32, "");
  oriole_ease_make(EASE_SHARP, 0, 0, 0, 1);
  K_DP_POWER = oriole_spring(0.25f, 1);
  K_DP_LOCK = oriole_spring(0.4f, 1);
  dp_mid_lock = oriole_text_mid(ic18, "\uf023");
  oriole_spring_set(dp_lock, dp_locked);  // started locked (flash) — dim and without touches from the first frame
  oriole_touch_on(!dp_locked);
  dp_sent = dp_start_level();  // oriole_begin starts at this brightness (dp_sl: settings_load)
}

static void dp_lock_set(bool on) {
  dp_locked = on, dp_lock.target = on;
  oriole_touch_on(!on);
  LOG("%s\n", on ? "locked" : "unlocked");
}

// Every frame, whatever is on screen: the catch-up, the brightness and the lock (BOOT — power.h)
static void display_tick(float dt) {
  // PWR: a click locks, a long press unlocks (a long press doesn't also click: the chip reports one or the other)
  const uint8_t pwr = oriole_pwr_event();
  if (pwr & ORIOLE_PWR_LONG) {
    if (dp_locked) dp_lock_set(false);
  } else if (pwr & ORIOLE_PWR_CLICK) {
    if (!dp_locked) dp_lock_set(true);
  }
  if (oriole_finger_blocked() && dp_locked) dp_shake_at = oriole_ms() | 1;
  oriole_spring_run(dp_power, K_DP_POWER, dt);
  oriole_spring_run(dp_lock, K_DP_LOCK, dt);
  slider_tick(dp_sl, dt);
  const float k = oriole_clamp01(dp_power.x), lk = oriole_clamp01(dp_lock.x), lv = dp_level(dp_sl.shown);
  ui_lock = lk;
  int level = (int)lroundf((lv + (DP_LEVEL[0] - lv) * lk) * k * k);  // the eye: the fade's end is the slow part
  if (fc_night_on && oriole_nav_on_face()) level = min(level, (int)DP_LEVEL[1]);  // night mode (face.h): low
  if (pw_bright >= 0) level = pw_bright;  // Always On
  if (level != dp_sent) oriole_brightness(level), dp_sent = level;
  if (dp_power.target < 0.5f && dp_power.x < 0.01f && oriole_display_is_on() && !pw_keep_lit)
    oriole_display_on(false), LOG("screen off\n");
}

static void display_enter() {
  slider_enter(dp_sl);
}

static void display_update(float dt) {
  slider_update(dp_sl, dt);
}

static void display_layout(const OrioleView &v) {
  text_mid(dp_head, t_title, "Display", dp_mid_head, CX, DP_HEAD_Y, 1, TEXT, 1, &oriole_black);
  text_mid(dp_label, t20, "Brightness", dp_mid_label, CX, DP_LABEL_Y, 1, DIM, 1, &oriole_black);
  text_mid(dp_hint, t20, "Press PWR to lock the screen", dp_mid_hint, CX, DP_HINT_Y, 1, DIM, 1, &oriole_black);
  dp_head.on_black = dp_label.on_black = dp_hint.on_black = true;
  oriole_view_commit(dp_head, v), oriole_view_commit(dp_label, v), oriole_view_commit(dp_hint, v);
  slider_layout(dp_sl, v);
}

static void display_draw() {
  oriole_item_draw(dp_head), oriole_item_draw(dp_label), oriole_item_draw(dp_hint);
  slider_draw(dp_sl);
}

// The lock icon over everything (laid out after oriole_nav_update, drawn after oriole_nav_draw). As in our Fade, never
// together with the time it replaces: the time fades out over the first half of the lock spring, the lock grows from
// 0.7 and fades in over the second; touched while locked — a short wiggle (10 Hz, 0.45 s, dying away); unlocking — the
// open lock fades out first, then the time comes back
static void display_overlay_layout() {
  const float a = oriole_clamp01(dp_lock.x * 2 - 1), t = dp_shake_at ? (oriole_ms() - dp_shake_at) / 1000.0f : 9;
  const float shake = t < 0.45f ? 5 * sinf(t * 2 * ORIOLE_PI * 10) * (1 - t / 0.45f) : 0;
  text_mid(dp_lock_ic, ic18, dp_locked ? "\uf023" : "\uf09c", dp_mid_lock, CX + shake, DP_LOCK_Y, 0.7f + 0.3f * a,
           0xC6C6C6, a > 0.01f ? a : 0, &oriole_black);
  dp_lock_ic.on_black = true;
  if (shake != 0) oriole_subpixel = true;
  oriole_item_commit(dp_lock_ic);
}

static void display_overlay_draw() {
  oriole_item_draw(dp_lock_ic);
}

// Static: no vertical gestures of its own — to the face either way (down from the top or up from the bottom edge)
static const OrioleScreen DISPLAY_SCREEN = {display_enter, display_update, display_layout, display_draw, false,
                                          ORIOLE_HOME_BOTH};
