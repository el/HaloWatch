// games.h — the Level, and small games for the round screen:
//   Pop   — bubbles grow and shrink at random places; tap them before they are gone (30 s)
//   Tilt  — tilt the watch to roll the ball onto the target (30 s)
//   Orbit — a dot circles the ring: tap while it is inside the amber arc; faster each time
//   Echo  — four pads light up with their tones, one more each round: tap them back in order
//   Quick — wait for green, then tap: the reaction time (the best is the lowest)
//   Dodge — tilt to keep the ball away from the rocks crossing the arena: seconds survived
// Tilt comes from the accelerometer, read in loop() while one of these screens is up (I2C does not belong in the
// frame task). The best scores are kept in flash (se_prefs, written from loop()). Included from Oriole.ino after
// apps.h.

// ---------- Tilt, from loop() ----------
// The board's axes against the screen, measured: the left edge lifted gives +y, the top edge lifted +x (screen x
// grows to the right, y downwards: tl_x / tl_y point downhill)
#define TL_SX (1.0f)
#define TL_SY (1.0f)
static volatile float tl_x, tl_y, tl_z = 1;  // g, smoothed; tl_x / tl_y — along the screen's x and y
static volatile uint32_t tl_want_ms;
static float tl_ox, tl_oy;  // "level" is here (a tap anywhere on Level or in a Tilt round): until the screen is left

static void tilt_zero() {  // the tilt now counts as level
  tl_ox = tl_x, tl_oy = tl_y;
}
static float tilt_dx() {
  return tl_x - tl_ox;
}
static float tilt_dy() {
  return tl_y - tl_oy;
}

static void tilt_want() {  // the frame task: a screen needs the tilt now
  tl_want_ms = millis();
}

static void tilt_poll() {  // loop(): ~50 times a second while wanted
  static uint32_t at;
  static bool data;  // wake-on-motion off: the chip gives readings (Halo: halo_motion_data)
  const bool want = millis() - tl_want_ms < 300;
  if (want != data) data = want, halo_motion_data(want);  // off again a moment after the screen is left
  if (!want || millis() - at < 20) return;
  at = millis();
  float x, y, z;
  if (!halo_accel(&x, &y, &z)) return;
  const float k = 0.35f;
  tl_x += (TL_SX * y - tl_x) * k, tl_y += (TL_SY * x - tl_y) * k, tl_z += (z - tl_z) * k;
}

// ---------- Level: a bubble that floats to the high side; green within 1° ----------
struct LevelUi {
  HaloItem ring, target, h, vline, bubble, deg, hint;
  char deg_s[8];
  float bx, by;  // the bubble, smoothed (px from the middle)
};
static LevelUi &lv = psram_new<LevelUi>();
static float lv_mid_deg;

static void level_layout(const HaloView &v) {
  tilt_want();
  const float gx = tilt_dx(), gy = tilt_dy(), ang = atan2f(hypotf(gx, gy), fabsf(tl_z)) * 180 / HALO_PI;
  float tx = -gx * 420, ty = -gy * 420;  // the bubble goes up the slope
  const float d = hypotf(tx, ty);
  if (d > 150) tx *= 150 / d, ty *= 150 / d;
  lv.bx += (tx - lv.bx) * 0.25f, lv.by += (ty - lv.by) * 0.25f;
  const bool flat = ang < 1;
  snprintf(lv.deg_s, sizeof(lv.deg_s), "%d°", (int)lroundf(ang));
  lv.ring.kind = HI_RING, lv.ring.x = lv.ring.y = CX, lv.ring.r = 190, lv.ring.w = 3, lv.ring.v = 1, lv.ring.scale = 1;
  lv.ring.color = lv.ring.color2 = 0x5F6368, lv.ring.alpha = 1, lv.ring.on_black = true;
  lv.target.kind = HI_RING, lv.target.x = lv.target.y = CX, lv.target.r = 36, lv.target.w = 3, lv.target.v = 1;
  lv.target.scale = 1, lv.target.color = lv.target.color2 = flat ? 0x8BE3B9 : 0x9AA0A6, lv.target.alpha = 1;
  lv.target.on_black = true;
  for (HaloItem *l : {&lv.h, &lv.vline}) l->kind = HI_LINE, l->r = 1, l->scale = 1, l->color = 0x3C4043, l->alpha = 1;
  lv.h.x = CX - 190, lv.h.w = CX + 190, lv.h.y = lv.h.h = CX;
  lv.vline.y = CX - 190, lv.vline.h = CX + 190, lv.vline.x = lv.vline.w = CX;
  lv.bubble.kind = HI_CIRCLE, lv.bubble.x = CX + lv.bx, lv.bubble.y = CX + lv.by, lv.bubble.r = 28, lv.bubble.scale = 1;
  lv.bubble.color = flat ? 0x8BE3B9 : 0xE8EAED, lv.bubble.alpha = 1, lv.bubble.on_black = false;
  halo_subpixel = true;  // it glides
  text_mid(lv.deg, n48, lv.deg_s, lv_mid_deg, CX, 100, 1, flat ? 0x8BE3B9 : TEXT, 1, &halo_black);
  const bool zeroed = tl_ox != 0 || tl_oy != 0;
  text_mid(lv.hint, t20, zeroed ? "Leveled here: tap to redo" : "Tap to level here", ap_mid_t20, CX, 448, 1, DIM, 1,
           &halo_black);
  lv.deg.on_black = lv.hint.on_black = true;
  for (HaloItem *it : {&lv.ring, &lv.h, &lv.vline, &lv.target, &lv.bubble, &lv.deg, &lv.hint}) halo_view_commit(*it, v);
}

static void level_update(float) {  // a tap anywhere: level is here now
  const HaloFinger &f = halo_finger;
  if (f.tap && !f.owner) tilt_zero();
}

static void level_draw() {
  for (HaloItem *it : {&lv.ring, &lv.h, &lv.vline, &lv.target, &lv.bubble, &lv.deg, &lv.hint}) halo_item_draw(*it);
}
static const HaloScreen LEVEL_SCREEN = {[] { tl_ox = tl_oy = 0; }, level_update, level_layout, level_draw, false,
                                        HALO_HOME_BOTH};

// ---------- The games: one table, one frame (Start, the score, game over, the best one kept) ----------
enum : uint8_t { GM_IDLE, GM_PLAY, GM_OVER };
enum : uint8_t { G_POP, G_TILT, G_ORBIT, G_ECHO, G_QUICK, G_DODGE, G_N };
struct GameDef {
  const char *name, *hint, *key;  // the key — its best score in flash
  bool timed;                     // a 30 s round (or until a mistake)
  bool low;                       // the lower the better (Quick: milliseconds)
};
static const GameDef GD[G_N] = {
    {"Pop", "Tap the bubbles", "g_pop", true, false},       {"Tilt", "Tilt to roll the ball", "g_tilt", true, false},
    {"Orbit", "Tap when the dot is in the arc", "g_orbit", false, false},
    {"Echo", "Watch, then tap the same", "g_echo", false, false},
    {"Quick", "Tap when it turns green", "g_quick", false, true},
    {"Dodge", "Tilt to miss the rocks", "g_dodge", false, false},
};
#define GM_ROUND_MS 30000
static int gm_best[G_N];
static volatile bool gm_dirty;
static float gm_mid_big;

struct GameUi {
  uint8_t state;
  int score;
  uint32_t end_ms;
  Button start;
  HaloItem title, line1, line2, score_t, clock;
  char title_s[16], line1_s[32], line2_s[24], score_s[12];
};

static void gm_begin(GameUi &g) {
  g.state = GM_PLAY, g.score = 0, g.end_ms = millis() + GM_ROUND_MS;
}

static void gm_finish(GameUi &g, int which) {  // over: a new best? a ding
  g.state = GM_OVER;
  const bool better = GD[which].low ? g.score > 0 && (!gm_best[which] || g.score < gm_best[which])
                                    : g.score > gm_best[which];
  if (better) gm_best[which] = g.score, gm_dirty = true;
  ai_chime_mode = 1, ai_chime_until = millis() + 300;  // gemini.h
}

static bool gm_tick(GameUi &g, int which) {  // true while playing; a timed round ends by itself
  if (g.state != GM_PLAY) return false;
  if (GD[which].timed && (int32_t)(millis() - g.end_ms) >= 0) return gm_finish(g, which), false;
  return true;
}

static bool gm_update(GameUi &g, float dt) {  // the Start button, when not playing; true — a round just began
  g.start.x = CX, g.start.y = 382, g.start.w = 190, g.start.h = 72, g.start.primary = true;
  if (btn_update(g.start, dt, g.state != GM_PLAY) && g.state != GM_PLAY) return gm_begin(g), true;
  return false;
}

static void gm_score_text(char *s, size_t n, int which, int v) {
  snprintf(s, n, GD[which].low ? "%d ms" : "%d", v);
}

static void gm_layout(GameUi &g, int which, const HaloView &v, bool big = true) {
  const GameDef &d = GD[which];
  const bool play = g.state == GM_PLAY;
  gm_score_text(g.score_s, sizeof(g.score_s), which, g.score);
  text_mid(g.score_t, n48, g.score_s, gm_mid_big, CX, 72, 1, TEXT, play && big ? 1 : 0, &halo_black);
  const char *over = d.timed ? "Time's up" : d.low ? (g.score ? "Your time" : "Too soon") : "Game over";
  strlcpy(g.title_s, g.state == GM_OVER ? over : d.name, sizeof(g.title_s));
  if (g.state == GM_OVER) {
    char sc[16];
    gm_score_text(sc, sizeof(sc), which, g.score);
    snprintf(g.line1_s, sizeof(g.line1_s), d.low ? "%s" : "Score %s", g.score || !d.low ? sc : "Wait for green");
  } else {
    strlcpy(g.line1_s, d.hint, sizeof(g.line1_s));
  }
  if (gm_best[which]) {
    char b[16];
    gm_score_text(b, sizeof(b), which, gm_best[which]);
    snprintf(g.line2_s, sizeof(g.line2_s), "Best %s", b);
  } else {
    strlcpy(g.line2_s, "No best yet", sizeof(g.line2_s));
  }
  const float ia = play ? 0 : 1;
  text_mid(g.title, t_title, g.title_s, ap_mid_head, CX, 150, 1, TEXT, ia, &halo_black);
  text_mid(g.line1, t24, g.line1_s, ap_mid_t24, CX, 214, 1, g.state == GM_OVER ? 0xF7A8C4 : DIM, ia, &halo_black);
  text_mid(g.line2, t20, g.line2_s, ap_mid_t20, CX, 260, 1, DIM, ia, &halo_black);
  g.score_t.on_black = g.title.on_black = g.line1.on_black = g.line2.on_black = true;
  for (HaloItem *it : {&g.score_t, &g.title, &g.line1, &g.line2}) halo_view_commit(*it, v);
  // A timed round: the time left as a ring at the edge (in 1/360 steps)
  const float left = play && d.timed ? halo_clamp01((int32_t)(g.end_ms - millis()) / (float)GM_ROUND_MS) : 0;
  g.clock.kind = HI_RING, g.clock.x = g.clock.y = CX, g.clock.r = 226, g.clock.w = 6, g.clock.scale = 1;
  g.clock.v = floorf(left * 360) / 360, g.clock.color = 0xF7A8C4, g.clock.color2 = FACE_TRACK;
  g.clock.alpha = play && d.timed ? 1 : 0, g.clock.on_black = true;
  halo_view_commit(g.clock, v);
  btn_layout(g.start, g.state == GM_OVER ? "Again" : "Start", v, ia);
}

static void gm_draw(const GameUi &g) {
  for (const HaloItem *it : {&g.clock, &g.score_t, &g.title, &g.line1, &g.line2}) halo_item_draw(*it);
  btn_draw(g.start);
}

static float rnd01() {
  return (esp_random() & 0xFFFF) / 65535.0f;
}

// ---------- Pop ----------
#define POP_N 4
#define POP_LIFE 1.8f
static const uint32_t POP_COLOR[] = {0x8AB4F8, 0xF7A8C4, 0x8BE3B9, 0xFFC56B, 0xC9B6FF};
struct PopUi {
  GameUi g;
  struct { float x, y, t; bool on; uint32_t color; } b[POP_N];
  float next;  // seconds to the next bubble
  HaloItem it[POP_N];
};
static PopUi &pp = psram_new<PopUi>();

static float pop_r(float t) {  // a bubble grows and shrinks over its life
  return 46 * sinf(HALO_PI * halo_clamp01(t / POP_LIFE));
}

static void pop_update(float dt) {
  GameUi &g = pp.g;
  gm_update(g, dt);
  if (!gm_tick(g, G_POP)) {
    for (auto &b : pp.b) b.on = false;
    return;
  }
  const HaloFinger &f = halo_finger;
  for (auto &b : pp.b) {
    if (!b.on) continue;
    b.t += dt;
    if (b.t >= POP_LIFE) b.on = false;  // gone, no point
    else if (f.pressed && hypotf(f.x0 - b.x, f.y0 - b.y) < pop_r(b.t) + 12) b.on = false, g.score++;
  }
  pp.next -= dt;
  if (pp.next <= 0) {
    pp.next = 0.35f + rnd01() * 0.4f;
    for (auto &b : pp.b)
      if (!b.on) {
        const float a = rnd01() * 2 * HALO_PI, r = 30 + rnd01() * 120;
        b.x = CX + r * cosf(a), b.y = CX + 20 + r * sinf(a), b.t = 0, b.on = true;
        b.color = POP_COLOR[esp_random() % 5];
        break;
      }
  }
}

static void pop_layout(const HaloView &v) {
  for (int i = 0; i < POP_N; i++) {
    HaloItem &c = pp.it[i];
    const auto &b = pp.b[i];
    c.kind = HI_CIRCLE, c.x = b.x, c.y = b.y, c.r = b.on ? pop_r(b.t) : 0, c.scale = 1, c.color = b.color;
    c.alpha = b.on && c.r > 1 ? 1 : 0, c.on_black = true;
    halo_view_commit(c, v);
  }
  gm_layout(pp.g, G_POP, v);
}

static void pop_draw() {
  for (auto &c : pp.it) halo_item_draw(c);
  gm_draw(pp.g);
}
static const HaloScreen POP_SCREEN = {[] { pp.g.state = GM_IDLE; }, pop_update, pop_layout, pop_draw, false,
                                      HALO_HOME_BOTH};

// ---------- Tilt ----------
#define TB_ARENA 206.0f  // the ball stays inside this radius
#define TB_R 15.0f
struct TiltUi {
  GameUi g;
  float x, y, vx, vy, tx, ty;  // the ball and the target, px from the middle
  HaloItem arena, ball, target;
};
static TiltUi &tb = psram_new<TiltUi>();

static void tb_target() {
  const float a = rnd01() * 2 * HALO_PI, r = 40 + rnd01() * 120;
  tb.tx = r * cosf(a), tb.ty = r * sinf(a);
}

// The ball by the tilt (downhill), kept inside the arena with a soft bounce; shared with Dodge
static void ball_step(float &x, float &y, float &vx, float &vy, float r, float dt) {
  vx += tilt_dx() * 1400 * dt, vy += tilt_dy() * 1400 * dt;
  const float damp = powf(0.35f, dt);
  vx *= damp, vy *= damp;
  x += vx * dt, y += vy * dt;
  const float d = hypotf(x, y), lim = TB_ARENA - r;
  if (d > lim) {  // the wall: back inside, the velocity's outward part reflected (and softened)
    const float nx = x / d, ny = y / d, vn = vx * nx + vy * ny;
    x = nx * lim, y = ny * lim;
    if (vn > 0) vx -= 1.6f * vn * nx, vy -= 1.6f * vn * ny;
  }
}

static void tilt_update(float dt) {
  GameUi &g = tb.g;
  if (gm_update(g, dt)) return tb.x = tb.y = tb.vx = tb.vy = 0, tb_target();  // the Start tap is not a game tap
  if (!gm_tick(g, G_TILT)) return;
  if (halo_finger.tap && !halo_finger.owner) tilt_zero();  // a tap anywhere: level here (the ball stays put)
  ball_step(tb.x, tb.y, tb.vx, tb.vy, TB_R, dt);
  if (hypotf(tb.x - tb.tx, tb.y - tb.ty) < TB_R + 17) g.score++, tb_target();
}

static void arena_ring(HaloItem &a, bool on, const HaloView &v) {
  a.kind = HI_RING, a.x = a.y = CX, a.r = TB_ARENA + 2, a.w = 3, a.v = 1, a.scale = 1;
  a.color = a.color2 = 0x3C4043, a.alpha = on ? 1 : 0, a.on_black = true;
  halo_view_commit(a, v);
}

static void tilt_layout(const HaloView &v) {
  tilt_want();
  const bool play = tb.g.state == GM_PLAY;
  arena_ring(tb.arena, play, v);
  tb.target.kind = HI_CIRCLE, tb.target.x = CX + tb.tx, tb.target.y = CX + tb.ty, tb.target.r = 17, tb.target.scale = 1;
  tb.target.color = 0xFFC56B, tb.target.alpha = play ? 1 : 0, tb.target.on_black = true;
  tb.ball.kind = HI_CIRCLE, tb.ball.x = CX + tb.x, tb.ball.y = CX + tb.y, tb.ball.r = TB_R, tb.ball.scale = 1;
  tb.ball.color = 0xE8EAED, tb.ball.alpha = play ? 1 : 0, tb.ball.on_black = false;
  if (play) halo_subpixel = true;
  for (HaloItem *it : {&tb.target, &tb.ball}) halo_view_commit(*it, v);
  gm_layout(tb.g, G_TILT, v);
}

static void tilt_draw() {
  for (HaloItem *it : {&tb.arena, &tb.target, &tb.ball}) halo_item_draw(*it);
  gm_draw(tb.g);
}
static const HaloScreen TILT_SCREEN = {[] { tb.g.state = GM_IDLE, tl_ox = tl_oy = 0; }, tilt_update, tilt_layout,
                                       tilt_draw, false, HALO_HOME_BOTH};

// ---------- Orbit: tap while the dot is inside the arc ----------
#define OB_R 196.0f
struct OrbitUi {
  GameUi g;
  float a, w, ta, tw;  // the dot's angle and speed (rad, rad/s); the target's middle and half-width (rad)
  int passes;          // how often the dot went past the target without a tap
  bool inside;
  HaloItem track, target, dot;
};
static OrbitUi &ob = psram_new<OrbitUi>();

static float ob_diff(float a, float b) {  // a − b, wrapped into −π..π
  float d = fmodf(a - b + 3 * HALO_PI, 2 * HALO_PI) - HALO_PI;
  return d;
}

static void ob_target() {  // somewhere ahead of the dot, at least a quarter turn
  ob.ta = ob.a + HALO_PI / 2 + rnd01() * HALO_PI, ob.tw = fmaxf(0.16f, 0.36f - ob.g.score * 0.012f);
  ob.passes = 0, ob.inside = false;
}

static void orbit_update(float dt) {
  GameUi &g = ob.g;
  if (gm_update(g, dt)) return ob.a = 0, ob.w = 2.2f, ob_target();  // the Start tap is not a game tap
  if (!gm_tick(g, G_ORBIT)) return;
  ob.a += ob.w * dt;
  const bool in = fabsf(ob_diff(ob.a, ob.ta)) < ob.tw;
  if (ob.inside && !in && ++ob.passes >= 2) return gm_finish(g, G_ORBIT);  // let it go past twice: over
  ob.inside = in;
  if (halo_finger.tap && !halo_finger.owner) {
    if (!in) return gm_finish(g, G_ORBIT);
    g.score++, ob.w *= 1.07f, ob_target();
  }
}

static void orbit_layout(const HaloView &v) {
  const bool play = ob.g.state == GM_PLAY;
  ob.track.kind = HI_RING, ob.track.x = ob.track.y = CX, ob.track.r = OB_R, ob.track.w = 10, ob.track.v = 1;
  ob.track.scale = 1, ob.track.color = ob.track.color2 = 0x2A2C30, ob.track.alpha = play ? 1 : 0, ob.track.on_black = true;
  ob.target.kind = HI_ARC, ob.target.x = ob.target.y = CX, ob.target.r = OB_R, ob.target.w = 16, ob.target.scale = 1;
  const float t0 = fmodf(ob.ta - ob.tw + 8 * HALO_PI, 2 * HALO_PI);
  ob.target.a0 = t0, ob.target.a1 = t0 + 2 * ob.tw, ob.target.color = 0xFFC56B, ob.target.alpha = play ? 1 : 0;
  ob.target.on_black = true;
  ob.dot.kind = HI_CIRCLE, ob.dot.x = CX + OB_R * sinf(ob.a), ob.dot.y = CX - OB_R * cosf(ob.a), ob.dot.r = 12;
  ob.dot.scale = 1, ob.dot.color = 0xE8EAED, ob.dot.alpha = play ? 1 : 0, ob.dot.on_black = false;
  if (play) halo_subpixel = true;
  for (HaloItem *it : {&ob.track, &ob.target, &ob.dot}) halo_view_commit(*it, v);
  gm_layout(ob.g, G_ORBIT, v);
}

static void orbit_draw() {
  for (HaloItem *it : {&ob.track, &ob.target, &ob.dot}) halo_item_draw(*it);
  gm_draw(ob.g);
}
static const HaloScreen ORBIT_SCREEN = {[] { ob.g.state = GM_IDLE; }, orbit_update, orbit_layout, orbit_draw, false,
                                        HALO_HOME_BOTH};

// ---------- Echo: watch the pads light up, then tap them back ----------
#define EC_MAX 32
static const uint32_t EC_COLOR[4] = {0x8AB4F8, 0xF7A8C4, 0x8BE3B9, 0xFFC56B};
static const uint16_t EC_HZ[4] = {523, 659, 784, 988};  // C, E, G, B
struct EchoUi {
  GameUi g;
  uint8_t seq[EC_MAX];
  int len, pos;      // the sequence so far; where the player (or the playback) is in it
  bool showing;      // the watch plays it now
  float t;           // seconds into the current step of the playback
  int lit;           // the pad lit now (−1 none)
  uint32_t lit_until;
  HaloItem pad[4];
};
static EchoUi &ec = psram_new<EchoUi>();

static void ec_light(int p, uint32_t ms) {  // a pad lit, with its tone
  ec.lit = p, ec.lit_until = millis() + ms;
  ai_tone_hz = EC_HZ[p], ai_chime_mode = 2, ai_chime_until = millis() + ms;
}

static void ec_next_round() {
  if (ec.len < EC_MAX) ec.seq[ec.len++] = esp_random() % 4;
  ec.showing = true, ec.pos = 0, ec.t = -0.6f;  // a breath before it starts
}

#define EC_D 104.0f  // a pad's middle, along each diagonal, from the screen's middle
#define EC_R 70.0f
static float ec_x(int i) {  // pads clockwise from the top right
  return CX + (i == 0 || i == 1 ? EC_D : -EC_D);
}
static float ec_y(int i) {
  return CX + (i == 1 || i == 2 ? EC_D : -EC_D);
}
static int ec_pad_at(float x, float y) {  // the pad under a touch (−1 none)
  for (int i = 0; i < 4; i++)
    if (hypotf(x - ec_x(i), y - ec_y(i)) < EC_R + 16) return i;
  return -1;
}

static void echo_update(float dt) {
  GameUi &g = ec.g;
  if (gm_update(g, dt)) return ec.len = 0, ec.lit = -1, ec_next_round();
  if ((int32_t)(millis() - ec.lit_until) >= 0) ec.lit = -1;
  if (!gm_tick(g, G_ECHO)) return;
  if (ec.showing) {  // 0.45 s a step, 0.15 s between
    ec.t += dt;
    if (ec.t >= 0 && ec.pos < ec.len && ec.lit < 0 && ec.t >= 0.6f * ec.pos) ec_light(ec.seq[ec.pos], 450), ec.pos++;
    if (ec.pos >= ec.len && ec.lit < 0 && ec.t > 0.6f * ec.len) ec.showing = false, ec.pos = 0;
    return;
  }
  const HaloFinger &f = halo_finger;
  if (!f.pressed || f.owner) return;
  const int p = ec_pad_at(f.x0, f.y0);
  if (p < 0) return;
  ec_light(p, 250);
  if (p != ec.seq[ec.pos]) return gm_finish(g, G_ECHO);  // the wrong one
  if (++ec.pos >= ec.len) g.score = ec.len, ec_next_round();
}

static void echo_layout(const HaloView &v) {
  const bool play = ec.g.state == GM_PLAY;
  for (int i = 0; i < 4; i++) {
    HaloItem &a = ec.pad[i];
    const bool on = ec.lit == i;
    a.kind = HI_CIRCLE, a.x = ec_x(i), a.y = ec_y(i), a.r = on ? EC_R + 6 : EC_R, a.scale = 1;
    a.color = on ? EC_COLOR[i] : halo_dim(EC_COLOR[i], 80), a.alpha = play ? 1 : 0, a.on_black = true;
    halo_view_commit(a, v);
  }
  gm_layout(ec.g, G_ECHO, v, false);
}

static void echo_draw() {
  for (auto &a : ec.pad) halo_item_draw(a);
  gm_draw(ec.g);
}
static const HaloScreen ECHO_SCREEN = {[] { ec.g.state = GM_IDLE, ec.lit = -1; }, echo_update, echo_layout, echo_draw,
                                       false, HALO_HOME_BOTH};

// ---------- Quick: wait for green, then tap ----------
struct QuickUi {
  GameUi g;
  uint32_t go_at;  // when it turns green
  HaloItem disc, word;
};
static QuickUi &qk = psram_new<QuickUi>();

static void quick_update(float dt) {
  GameUi &g = qk.g;
  if (gm_update(g, dt)) return (void)(qk.go_at = millis() + 1500 + esp_random() % 2500);
  if (!gm_tick(g, G_QUICK)) return;
  const HaloFinger &f = halo_finger;
  if (!f.pressed || f.owner) return;
  const int32_t ms = (int32_t)(millis() - qk.go_at);
  g.score = ms < 0 ? 0 : ms;  // before green: too soon (no score)
  gm_finish(g, G_QUICK);
}

static void quick_layout(const HaloView &v) {
  const bool play = qk.g.state == GM_PLAY, go = play && (int32_t)(millis() - qk.go_at) >= 0;
  qk.disc.kind = HI_CIRCLE, qk.disc.x = qk.disc.y = CX, qk.disc.r = 130, qk.disc.scale = 1;
  qk.disc.color = go ? 0x34A853 : 0x3C4043, qk.disc.alpha = play ? 1 : 0, qk.disc.on_black = true;
  text_mid(qk.word, t_title, go ? "Tap!" : "Wait...", ap_mid_head, CX, CX, 1, TEXT, play ? 1 : 0, &qk.disc);
  halo_view_commit(qk.disc, v), halo_view_commit(qk.word, v);
  gm_layout(qk.g, G_QUICK, v, false);
}

static void quick_draw() {
  halo_item_draw(qk.disc), halo_item_draw(qk.word);
  gm_draw(qk.g);
}
static const HaloScreen QUICK_SCREEN = {[] { qk.g.state = GM_IDLE; }, quick_update, quick_layout, quick_draw, false,
                                        HALO_HOME_BOTH};

// ---------- Dodge: tilt the ball away from the rocks; seconds survived ----------
#define DG_N 6
struct DodgeUi {
  GameUi g;
  float x, y, vx, vy, t, next;  // the ball; seconds survived; to the next rock
  struct { float x, y, vx, vy, r; bool on; } rk[DG_N];
  HaloItem arena, ball, rock[DG_N];
};
static DodgeUi &dg = psram_new<DodgeUi>();

static void dodge_update(float dt) {
  GameUi &g = dg.g;
  if (gm_update(g, dt)) {  // the Start tap is not a game tap
    dg.x = dg.y = dg.vx = dg.vy = dg.t = 0, dg.next = 1;
    for (auto &r : dg.rk) r.on = false;
    return;
  }
  if (!gm_tick(g, G_DODGE)) return;
  if (halo_finger.tap && !halo_finger.owner) tilt_zero();
  dg.t += dt, g.score = (int)dg.t;
  ball_step(dg.x, dg.y, dg.vx, dg.vy, TB_R, dt);
  const float speed = 110 + dg.t * 6;  // faster as it goes on
  dg.next -= dt;
  for (auto &r : dg.rk) {
    if (!r.on) {
      if (dg.next > 0) continue;
      dg.next = fmaxf(0.35f, 1.1f - dg.t * 0.02f);
      const float a = rnd01() * 2 * HALO_PI, aim = a + HALO_PI + (rnd01() - 0.5f) * 1.2f;  // across, roughly
      r.x = 215 * cosf(a), r.y = 215 * sinf(a), r.vx = speed * cosf(aim), r.vy = speed * sinf(aim);
      r.r = 12 + rnd01() * 10, r.on = true;
      continue;
    }
    r.x += r.vx * dt, r.y += r.vy * dt;
    if (hypotf(r.x, r.y) > 240) r.on = false;  // out of the circle
    else if (hypotf(r.x - dg.x, r.y - dg.y) < r.r + TB_R - 2) return gm_finish(g, G_DODGE);
  }
}

static void dodge_layout(const HaloView &v) {
  tilt_want();
  const bool play = dg.g.state == GM_PLAY;
  arena_ring(dg.arena, play, v);
  dg.ball.kind = HI_CIRCLE, dg.ball.x = CX + dg.x, dg.ball.y = CX + dg.y, dg.ball.r = TB_R, dg.ball.scale = 1;
  dg.ball.color = 0x8AB4F8, dg.ball.alpha = play ? 1 : 0, dg.ball.on_black = false;
  halo_view_commit(dg.ball, v);
  for (int i = 0; i < DG_N; i++) {
    HaloItem &c = dg.rock[i];
    const auto &r = dg.rk[i];
    c.kind = HI_CIRCLE, c.x = CX + r.x, c.y = CX + r.y, c.r = r.r, c.scale = 1, c.color = 0x9AA0A6;
    c.alpha = play && r.on ? 1 : 0, c.on_black = false;
    halo_view_commit(c, v);
  }
  if (play) halo_subpixel = true;
  gm_layout(dg.g, G_DODGE, v);
}

static void dodge_draw() {
  halo_item_draw(dg.arena);
  for (auto &c : dg.rock) halo_item_draw(c);
  halo_item_draw(dg.ball);
  gm_draw(dg.g);
}
static const HaloScreen DODGE_SCREEN = {[] { dg.g.state = GM_IDLE, tl_ox = tl_oy = 0; }, dodge_update, dodge_layout,
                                        dodge_draw, false, HALO_HOME_BOTH};

// ---------- The Games list ----------
static const HaloScreen *const GM_SCREEN[G_N] = {&POP_SCREEN,  &TILT_SCREEN,  &ORBIT_SCREEN,
                                                 &ECHO_SCREEN, &QUICK_SCREEN, &DODGE_SCREEN};
static char gm_sub[G_N][24];
static CardRow gm_rows[G_N];
static CardList &gm_list = cl_new("Games", gm_rows, G_N);
CL_SCREEN(GAMES_SCREEN, gm_list);

static void games_build() {  // after settings_load (se_prefs) and apps_build (the fonts' middles)
  halo_command("tilt", [](const char *) {  // the accelerometer now (raw, g) and the smoothed tilt
    float x = 0, y = 0, z = 0;
    const bool ok = halo_accel(&x, &y, &z);
    Serial.printf("TILT %s raw %.3f %.3f %.3f, smoothed %.3f %.3f %.3f\n", ok ? "ok" : "no data", x, y, z, tl_x, tl_y,
                  tl_z);
  });
  lv_mid_deg = gm_mid_big = halo_text_mid(n48, "0123456789");
  for (int i = 0; i < G_N; i++) {
    gm_best[i] = se_prefs.getInt(GD[i].key, 0);
    gm_rows[i] = {GD[i].name, nullptr, SE_VALUE, nullptr, nullptr, nullptr,
                  [](int k) {
                    char b[16];
                    gm_score_text(b, sizeof(b), k, gm_best[k]);
                    snprintf(gm_sub[k], sizeof(gm_sub[k]), gm_best[k] ? "Best %s" : "Not played yet", b);
                    return (const char *)gm_sub[k];
                  },
                  [](int k) { halo_nav_open(*GM_SCREEN[k]); }};
  }
  cl_build(gm_list);
}

static void games_save_poll() {  // loop(): a new best into flash
  if (!gm_dirty) return;
  gm_dirty = false;
  for (int i = 0; i < G_N; i++) se_prefs.putInt(GD[i].key, gm_best[i]);
}
