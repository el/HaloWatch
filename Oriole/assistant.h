// assistant.h — the "Assistant" screen and push-to-talk on BOOT (the engine is gemini.h).
//
// Hold BOOT anywhere — even in Always On or asleep — and talk; let go and the answer is spoken. Pressing it on the
// face opens this screen over the face (Fade), in the gallery — as a card would; elsewhere (Weather, Settings) it
// works where you are, with a small dot at the bottom edge showing what the assistant is doing. Pressing BOOT while
// it speaks interrupts it and starts your next question.
//
// The screen: the clock along the top edge (as on Weather); an orb — its color says the state (blue — listening, violet
// — thinking, teal — speaking, gray — idle, red — a problem), its size follows your voice while you talk and the
// answer's while it speaks, and it breathes while waiting; under it what is happening, step by step ("Connecting to
// Wi-Fi…", "Connecting to Gemini…", "Listening…", "Sending… 40%" with a ring around the orb filling as the question
// goes out, "Thinking…", "Loading the answer…" when the reply runs dry, "Speaking", "Error: …"), then what you said (one dim line, the end of it) and the answer as live captions: its last three
// lines, growing as it is spoken. All on black: at rest nothing is sent to the display.
// Included from Oriole.ino after gemini.h and power.h.

#define AS_ORB_Y 150.0f
#define AS_ORB_R 50.0f
#define AS_STATUS_Y 240.0f
#define AS_YOU_Y 280.0f
#define AS_LINE_Y0 318.0f
#define AS_LINE_DY 34.0f
#define AS_LINES 3
#define AS_WRAP_W 312.0f  // caption width: what fits at the lowest line, with a margin to the screen's edge

static HaloItem as_clock, as_glow, as_orb, as_mic, as_status, as_you, as_line[AS_LINES], as_dot, as_send;
static char as_status_s[96], as_you_s[96], as_line_s[AS_LINES][96];
static float as_mid_status, as_mid_you, as_mid_line, as_mid_mic;
static HaloSpring as_size, as_glow_k;  // the orb's size (0..1 of the extra) and the glow
static HaloSpringK K_AS_SIZE;
static float as_rgb[3] = {0x5F, 0x63, 0x68};
static uint32_t as_txt_seen = ~0u;
static bool as_on_screen;  // the Assistant screen was laid out visible this frame (no dot then)
static int as_down, as_up;
static uint32_t as_mic_off_at;

static bool as_sending() {  // let go, the question still going out
  return ai_in_turn && !ai_hold && ring_avail(cap) > 0;
}
static float as_send_progress() {
  return ai_send_total ? 1 - halo_clamp01((float)ring_avail(cap) / ai_send_total) : 1;
}
static bool as_buffering() {  // speaking, but the reply ran dry and the speaker waits for more
  return ai_state == AI_SPEAKING && !ai_playing && !ai_turn_done;
}

static uint32_t as_state_color() {
  if (ai_hold) return 0x8AB4F8;
  if (as_sending()) return 0xA8C7FA;
  switch (ai_state) {
    case AI_CONNECTING: return 0x8AB4F8;
    case AI_LISTENING: return 0x8AB4F8;
    case AI_THINKING: return 0xC58AF9;
    case AI_SPEAKING: return 0x78D9EC;
    case AI_ERROR: return 0xF28B82;
    default: return 0x5F6368;
  }
}

static void as_status_text(char *s, size_t n) {
  const bool key = cfg.key[0], wifi = cfg.ssid[0];  // just "set or not": no lock (the frame task must not wait)
  const char *to = ai_net_wait ? "Wi-Fi" : "Gemini";
  if (ai_hold) snprintf(s, n, ai_state == AI_CONNECTING ? "Listening… (connecting to %s)" : "Listening…", to);
  else if (ai_state == AI_CONNECTING) snprintf(s, n, "Connecting to %s…", to);
  else if (as_sending()) snprintf(s, n, "Sending… %d%%", (int)(as_send_progress() * 100));
  else if (ai_state == AI_THINKING) snprintf(s, n, "Thinking…");
  else if (as_buffering()) snprintf(s, n, "Loading the answer…");
  else if (ai_state == AI_SPEAKING) snprintf(s, n, "Speaking (BOOT interrupts)");
  else if (ai_state == AI_ERROR) snprintf(s, n, "Error: %s", ai_error);
  else if (!wifi) snprintf(s, n, "Set up Wi-Fi in the web controller");
  else if (!key) snprintf(s, n, "Add a Gemini API key in the web controller");
  else snprintf(s, n, "Hold BOOT to talk");
  while (strlen(s) > 4 && halo_text_width(t20, s) > 400) strcpy(s + strlen(s) - 4, "…");  // fits the screen
}

// What the font can draw: curly quotes and dashes as ASCII, line breaks as spaces, other letters dropped
static void as_clean(const char *in, char *out, size_t n) {
  size_t k = 0;
  while (*in && k + 4 < n) {
    const char *p = in;
    const uint32_t c = halo_utf8_next(in);
    if (c == 0x2018 || c == 0x2019) out[k++] = '\'';
    else if (c == 0x201C || c == 0x201D) out[k++] = '"';
    else if (c == '\n' || c == '\t' || c == '\r') out[k++] = ' ';
    else if (c < 0x80 || halo_glyph(t24, c)) memcpy(out + k, p, in - p), k += in - p;
  }
  out[k] = 0;
}

// The answer as captions: greedy word wrap, the last AS_LINES lines; what the user said — its end, on one line
struct AsWrap {  // in PSRAM: the frame task's stack is small
  char said[1300], you[260], clean[1300], lines[24][96], test[200];
};
static AsWrap *as_w;

static void as_wrap() {
  if (!as_w) return;
  char *said = as_w->said, *you = as_w->you, *clean = as_w->clean, (*lines)[96] = as_w->lines;
  const size_t SAID = sizeof(as_w->said), YOU = sizeof(as_w->you), CLEAN = sizeof(as_w->clean);
  portENTER_CRITICAL(&ai_mux);
  strlcpy(said, ai_said, SAID), strlcpy(you, ai_you, YOU);
  as_txt_seen = ai_txt_ver;
  portEXIT_CRITICAL(&ai_mux);
  as_clean(you, clean, CLEAN);
  const char *y = clean;
  while (*y == ' ') y++;
  while (*y && halo_text_width(t20, y) > AS_WRAP_W + 40) {  // the end of it, from a word
    const char *sp = strchr(y + 1, ' ');
    y = sp ? sp + 1 : y + strlen(y);
  }
  snprintf(as_you_s, sizeof(as_you_s), "%s%s", y != clean && *y ? "…" : "", y);
  as_clean(said, clean, CLEAN);
  int nl = 0;
  lines[0][0] = 0;
  for (char *save = nullptr, *w = strtok_r(clean, " ", &save); w; w = strtok_r(nullptr, " ", &save)) {
    char *test = as_w->test;
    snprintf(test, sizeof(as_w->test), "%s%s%s", lines[nl % 24], lines[nl % 24][0] ? " " : "", w);
    if (lines[nl % 24][0] && halo_text_width(t24, test) > AS_WRAP_W) nl++, strlcpy(lines[nl % 24], w, 96);
    else strlcpy(lines[nl % 24], test, 96);
  }
  const int total = lines[nl % 24][0] ? nl + 1 : nl;
  for (int i = 0; i < AS_LINES; i++) {
    const int src = total - AS_LINES + i;
    strlcpy(as_line_s[i], src >= 0 && src >= total - 24 ? lines[src % 24] : "", sizeof(as_line_s[i]));
  }
}

static void assistant_build() {
  as_mid_status = halo_text_mid(t20, "Hold BOOT to talk");
  as_mid_you = halo_text_mid(t20, "Hello");
  as_mid_line = halo_text_mid(t24, "Hello");
  as_mid_mic = halo_text_mid(ic32, "");
  K_AS_SIZE = halo_spring(0.18f, 0.8f);
  as_w = (AsWrap *)heap_caps_malloc(sizeof(AsWrap), MALLOC_CAP_SPIRAM);
}

static void as_open();  // the screen, over the face or the gallery (after ASSISTANT, below)

static void as_press() {
  ai_short = false, ai_press_ms = millis(), ai_hold = true, ai_press_new = true;
  ai_capture = true;
  halo_mic_on(ai_mic);
  if (ai_playing || ring_avail(play)) ai_play_flush = true;  // barge-in: quiet at once
  net_poke();
  if (ai_task_h) xTaskNotifyGive(ai_task_h);
  as_open();
  LOG("BOOT: talking\n");
}

static void as_release() {
  ai_short = millis() - ai_press_ms < 300;  // before ai_hold: the session task reads them in that order
  ai_send_total = ring_avail(cap);          // what is still to send: the base of "Sending… NN%"
  __sync_synchronize();
  ai_hold = false, ai_capture = false;
  as_mic_off_at = halo_ms() + 300;
  if (ai_task_h) xTaskNotifyGive(ai_task_h);
}

// Every frame, after power_tick (BOOT wakes the board there first): BOOT held is talking
static void assistant_tick(float dt) {
  const bool down = digitalRead(0) == LOW;
  if (down) as_down++, as_up = 0;
  else as_up++, as_down = 0;
  if (!ai_hold && as_down >= 2 && pw_mode == PW_ACTIVE) as_press();
  if (ai_hold && as_up >= 2) as_release();
  if (!ai_hold && as_mic_off_at && (int32_t)(halo_ms() - as_mic_off_at) >= 0) {
    as_mic_off_at = 0;
    if (!ai_capture) halo_mic_off();
  }
  // The orb: size and color follow the state
  const float lvl = ai_hold ? ai_in_level : ai_state == AI_SPEAKING ? ai_out_level : 0;
  const float breathe = ai_state == AI_THINKING || ai_state == AI_CONNECTING ? 0.5f + 0.5f * sinf(halo_ms() / 1000.0f * 2 * HALO_PI * 0.8f) : 0;
  as_size.target = fminf(1, sqrtf(halo_clamp01(lvl)) * 1.2f) * 0.9f + breathe * 0.25f;
  halo_spring_run(as_size, K_AS_SIZE, dt);
  as_glow_k.target = ai_hold || ai_state == AI_SPEAKING || ai_state == AI_THINKING ? 1 : 0;
  halo_spring_run(as_glow_k, K_AS_SIZE, dt);
  const uint32_t c = as_state_color();
  const float k = 1 - expf(-dt / 0.12f);
  for (int i = 0; i < 3; i++) as_rgb[i] += ((c >> (16 - 8 * i) & 255) - as_rgb[i]) * k;
  if (ai_txt_ver != as_txt_seen) as_wrap();
  as_status_text(as_status_s, sizeof(as_status_s));
}

static uint32_t as_color() {
  return (uint32_t)lroundf(as_rgb[0]) << 16 | (uint32_t)lroundf(as_rgb[1]) << 8 | (uint32_t)lroundf(as_rgb[2]);
}

static void assistant_layout(const HaloView &v) {
  if (v.a > 0.5f) as_on_screen = true;
  // The clock along the top edge, as on Weather
  as_clock.kind = HI_MASK, as_clock.mask = &tt_mask, as_clock.color = 0xC6C6C6, as_clock.on_black = true;
  as_clock.on = &halo_black, as_clock.x = tt_x + tt_mask.w / 2.0f, as_clock.y = tt_y + tt_mask.h / 2.0f;
  as_clock.scale = 1, as_clock.alpha = tt_mask.bits ? 1 - halo_clamp01(ui_lock * 2) : 0;
  halo_view_commit(as_clock, v);
  // The orb and its glow; at rest exactly still (nothing is redrawn)
  const float s = halo_clamp01(as_size.x), g = halo_clamp01(as_glow_k.x);
  const uint32_t col = as_color();
  as_glow.kind = HI_CIRCLE, as_glow.x = CX, as_glow.y = AS_ORB_Y, as_glow.r = AS_ORB_R + 8 + 30 * s, as_glow.scale = 1;
  as_glow.color = col, as_glow.alpha = g > 0.01f ? 0.22f * g : 0, as_glow.on_black = true;
  as_orb.kind = HI_CIRCLE, as_orb.x = CX, as_orb.y = AS_ORB_Y, as_orb.r = AS_ORB_R * (1 + 0.12f * s), as_orb.scale = 1;
  as_orb.color = col, as_orb.alpha = 1, as_orb.on_black = true;
  halo_view_commit(as_glow, v), halo_view_commit(as_orb, v);
  text_mid(as_mic, ic32, "", as_mid_mic, CX, AS_ORB_Y, 1, 0x1B1B1F, 1, &as_orb);
  // Sending: a ring around the orb fills as the question goes out
  as_send.kind = HI_RING, as_send.x = CX, as_send.y = AS_ORB_Y, as_send.r = AS_ORB_R + 22, as_send.w = 6;
  as_send.v = as_send_progress(), as_send.color = 0xA8C7FA, as_send.color2 = 0x2A2C30, as_send.scale = 1;
  as_send.alpha = as_sending() ? 1 : 0, as_send.on_black = true;
  halo_view_commit(as_send, v);
  halo_view_commit(as_mic, v);
  // Status, what you said, the answer
  text_mid(as_status, t20, as_status_s, as_mid_status, CX, AS_STATUS_Y, 1, ai_state == AI_ERROR ? 0xF28B82 : DIM, 1,
           &halo_black);
  text_mid(as_you, t20, as_you_s, as_mid_you, CX, AS_YOU_Y, 1, 0x8E9099, as_you_s[0] ? 1 : 0, &halo_black);
  as_status.on_black = as_you.on_black = true;
  halo_view_commit(as_status, v), halo_view_commit(as_you, v);
  for (int i = 0; i < AS_LINES; i++) {
    text_mid(as_line[i], t24, as_line_s[i], as_mid_line, CX, AS_LINE_Y0 + i * AS_LINE_DY, 1, TEXT,
             as_line_s[i][0] ? 1 : 0, &halo_black);
    as_line[i].on_black = true;
    halo_view_commit(as_line[i], v);
  }
}

static void assistant_draw() {
  halo_item_draw(as_clock);
  halo_item_draw(as_glow), halo_item_draw(as_send), halo_item_draw(as_orb), halo_item_draw(as_mic);
  halo_item_draw(as_status), halo_item_draw(as_you);
  for (auto &l : as_line) halo_item_draw(l);
}

// Elsewhere: a dot at the bottom edge while the assistant is busy (laid out after halo_nav_update)
static void assistant_overlay_layout() {
  const bool show = ai_busy() && !as_on_screen;
  as_on_screen = false;  // the screen sets it again in its layout next frame
  const float s = halo_clamp01(as_size.x);
  as_dot.kind = HI_CIRCLE, as_dot.x = CX, as_dot.y = 446, as_dot.r = 7 + 4 * s, as_dot.scale = 1;
  as_dot.color = as_color(), as_dot.alpha = show ? 1 : 0, as_dot.on_black = true;
  halo_item_commit(as_dot);
}

static void assistant_overlay_draw() {
  halo_item_draw(as_dot);
}

// No gestures of its own: to the face either way, back — from the left edge (or up from the bottom over the face)
static const HaloScreen ASSISTANT = {nullptr, nullptr, assistant_layout, assistant_draw, false, HALO_HOME_BOTH};

static void as_open() {
  if (halo_nav_on_face()) halo_nav_face_open(ASSISTANT, HALO_FX_FADE, CX, AS_ORB_Y, 2 * AS_ORB_R, 2 * AS_ORB_R);
  else if (halo_nav_at_home()) halo_nav_open(ASSISTANT);
}
