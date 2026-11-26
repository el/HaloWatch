// config.h — the watch's settings that the web controller (web/controller.html, over USB in Chrome) reads and writes:
// Wi-Fi, the Gemini API key and the assistant's model, voice and prompt, the weather cities and units, the clock.
//
// Kept in flash (NVS namespace "hwcfg"), read in setup() before anything uses them. Changed only by the serial command
// `set` (it runs in loop(), so flash writes never land in the middle of a frame). Other tasks (Wi-Fi, the assistant,
// the weather) read them under cfg_lock(): a copy of what they need, taken when they start a connection.
//
// Serial protocol (115200, one line per command; replies are single lines that start with a tag, so the controller
// can tell them from the log):
//   cfg                 -> CFG {json}            all settings; secrets masked ("ai_key":"…AbCd", "wifi_pass_set":true)
//   set KEY VALUE       -> OK KEY | ERR KEY why  VALUE is the rest of the line; "\n" and "\\" escapes (the prompt)
//   status              -> STATUS {json}         Wi-Fi, clock, assistant, weather, battery, memory (status.h)
//   time EPOCH [OFFSET] -> OK time               set the clock (the controller sends the computer's), OFFSET —
//                                                minutes east of UTC, used while the time zone is "auto"
//   reboot
// Included from Oriole.ino first of the app's headers.
#include <Preferences.h>

struct Cfg {
  char ssid[33], pass[65];
  char key[128], model[96], voice[32], lang[16];
  char prompt[1200];
  bool search, auto_vad;
  int vol, gain, mic_ch, keep_s;
  char cities[160], tz[64];
  bool fahrenheit, clock24;
  int face_style, face_color;
  bool bt_on;
  int wx_every, net_idle;
};
static Cfg cfg;
static Preferences cfg_prefs;
static SemaphoreHandle_t cfg_mx;
static volatile uint32_t cfg_ver;  // bumped on every change: modules re-read what they use

#define CFG_DEFAULT_MODEL "gemini-3.1-flash-live-preview"
#define CFG_DEFAULT_PROMPT                                                                                         \
  "You are a friendly voice assistant on a small round smartwatch. Keep answers short and conversational: one to " \
  "three sentences, unless the user asks for more. No lists or markdown: your words are spoken aloud. Answer in "  \
  "the language the user speaks."

struct CfgLock {
  CfgLock() {
    if (cfg_mx) xSemaphoreTake(cfg_mx, portMAX_DELAY);
  }
  ~CfgLock() {
    if (cfg_mx) xSemaphoreGive(cfg_mx);
  }
};

enum : uint8_t { CF_STR, CF_BOOL, CF_INT };
struct CfgField {
  const char *name;  // in `set` and in the JSON; also the NVS key (NVS keys are at most 15 characters)
  uint8_t type;
  void *p;
  int size_or_min, max;  // CF_STR: buffer size; CF_INT: range
  bool secret;
  const char *def;  // default: the string, "1"/"0", or the number
};
static const CfgField CFG_FIELDS[] = {
    {"wifi_ssid", CF_STR, cfg.ssid, sizeof(cfg.ssid), 0, false, ""},
    {"wifi_pass", CF_STR, cfg.pass, sizeof(cfg.pass), 0, true, ""},
    {"ai_key", CF_STR, cfg.key, sizeof(cfg.key), 0, true, ""},
    {"ai_model", CF_STR, cfg.model, sizeof(cfg.model), 0, false, CFG_DEFAULT_MODEL},
    {"ai_voice", CF_STR, cfg.voice, sizeof(cfg.voice), 0, false, "Puck"},
    {"ai_lang", CF_STR, cfg.lang, sizeof(cfg.lang), 0, false, ""},
    {"ai_prompt", CF_STR, cfg.prompt, sizeof(cfg.prompt), 0, false, CFG_DEFAULT_PROMPT},
    {"ai_search", CF_BOOL, &cfg.search, 0, 0, false, "1"},
    {"ai_auto_vad", CF_BOOL, &cfg.auto_vad, 0, 0, false, "1"},  // gemini-3.1-flash-live closes manual turns: 1007
    {"ai_keep", CF_INT, &cfg.keep_s, 0, 3600, false, "120"},
    {"volume", CF_INT, &cfg.vol, 0, 100, false, "70"},
    {"mic_gain", CF_INT, &cfg.gain, 0, 37, false, "30"},
    {"mic_channel", CF_INT, &cfg.mic_ch, 0, 2, false, "0"},
    {"wx_cities", CF_STR, cfg.cities, sizeof(cfg.cities), 0, false, "San Francisco;New York;Boston;Seattle"},
    {"wx_fahrenheit", CF_BOOL, &cfg.fahrenheit, 0, 0, false, "1"},
    {"wx_every", CF_INT, &cfg.wx_every, 10, 360, false, "30"},
    {"tz", CF_STR, cfg.tz, sizeof(cfg.tz), 0, false, ""},
    {"clock24", CF_BOOL, &cfg.clock24, 0, 0, false, "0"},
    {"face_style", CF_INT, &cfg.face_style, 0, 5, false, "0"},  // face.h: FACE_STYLE
    {"face_color", CF_INT, &cfg.face_color, 0, 4, false, "0"},  // face.h: FACE_COLOR
    {"bt_on", CF_BOOL, &cfg.bt_on, 0, 0, false, "1"},            // ancs.h: iPhone notifications
    {"net_idle", CF_INT, &cfg.net_idle, 10, 3600, false, "60"},
};
#define CFG_N (int)(sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0]))
static_assert(sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0]) <= 32, "cfg_ui_dirty: a bit per field");

static void cfg_apply(const CfgField &f, const char *v) {  // the value into its field (no saving)
  if (f.type == CF_STR) strlcpy((char *)f.p, v, f.size_or_min);
  else if (f.type == CF_BOOL) *(bool *)f.p = !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on");
  else *(int *)f.p = constrain(atoi(v), f.size_or_min, f.max);
}

static void cfg_load() {
  cfg_mx = xSemaphoreCreateMutex();
  cfg_prefs.begin("hwcfg", false);
  for (const CfgField &f : CFG_FIELDS) {
    cfg_apply(f, f.def);
    if (!cfg_prefs.isKey(f.name)) continue;
    if (f.type == CF_STR) cfg_prefs.getString(f.name, (char *)f.p, f.size_or_min);
    else if (f.type == CF_BOOL) *(bool *)f.p = cfg_prefs.getBool(f.name);
    else *(int *)f.p = constrain((int)cfg_prefs.getInt(f.name), f.size_or_min, f.max);
  }
}

// ---------- JSON out: a growing buffer in PSRAM ----------
struct JBuf {
  char *p = nullptr;
  size_t n = 0, cap = 0;
  ~JBuf() {
    free(p);
  }
  void put(const char *s, size_t k) {
    if (n + k + 1 > cap) {
      const size_t c = max(cap * 2, n + k + 256);
      char *q = (char *)heap_caps_realloc(p, c, MALLOC_CAP_SPIRAM);
      if (!q) return;
      p = q, cap = c;
    }
    memcpy(p + n, s, k), n += k, p[n] = 0;
  }
  void put(const char *s) {
    put(s, strlen(s));
  }
  void f(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    put(b, min(k, (int)sizeof(b) - 1));
  }
  void str(const char *s) {  // a JSON string, escaped
    put("\"", 1);
    for (; *s; s++) {
      const uint8_t c = *s;
      if (c == '"' || c == '\\') put("\\", 1), put((const char *)&c, 1);
      else if (c == '\n') put("\\n", 2);
      else if (c == '\r') put("\\r", 2);
      else if (c == '\t') put("\\t", 2);
      else if (c < 0x20) f("\\u%04x", c);
      else put((const char *)&c, 1);
    }
    put("\"", 1);
  }
  void key(const char *k) {  // ,"k": (no comma before the first key)
    if (n && p[n - 1] != '{' && p[n - 1] != '[') put(",", 1);
    str(k), put(":", 1);
  }
  void line(const char *tag) {  // out to the serial port as one line: TAG {json}
    if (!p) return;
    Serial.printf("%s ", tag);
    Serial.write((const uint8_t *)p, n);
    Serial.write('\n');
  }
};

static void cfg_json(JBuf &j) {
  CfgLock l;
  j.put("{");
  for (const CfgField &f : CFG_FIELDS) {
    if (f.type == CF_STR && f.secret) {  // a secret: whether it is set, and for the key its last 4 characters
      const char *s = (const char *)f.p;
      char k[24];
      snprintf(k, sizeof(k), "%s_set", f.name);
      j.key(k), j.put(*s ? "true" : "false");
      if (*s && !strcmp(f.name, "ai_key")) {
        const size_t n = strlen(s);
        j.key(f.name), j.str(n > 4 ? s + n - 4 : "");
      }
      continue;
    }
    j.key(f.name);
    if (f.type == CF_STR) j.str((const char *)f.p);
    else if (f.type == CF_BOOL) j.put(*(bool *)f.p ? "true" : "false");
    else j.f("%d", *(int *)f.p);
  }
  j.put("}");
}

static void cfg_unescape(char *s) {  // "\n" -> newline, "\\" -> "\", in place
  char *o = s;
  for (; *s; s++) {
    if (*s == '\\' && s[1] == 'n') *o++ = '\n', s++;
    else if (*s == '\\' && s[1] == '\\') *o++ = '\\', s++;
    else *o++ = *s;
  }
  *o = 0;
}

static void cfg_changed(const char *name);  // the app reacts (Oriole.ino: volume, mic, weather, time zone)

static void cfg_save(const CfgField &f) {  // into flash, then the app reacts; from loop() only
  if (f.type == CF_STR) cfg_prefs.putString(f.name, (const char *)f.p);
  else if (f.type == CF_BOOL) cfg_prefs.putBool(f.name, *(bool *)f.p);
  else cfg_prefs.putInt(f.name, *(int *)f.p);
  cfg_ver++;
  LOG("setting %s %s\n", f.name, f.secret ? "(secret) saved" : "saved");
  cfg_changed(f.name);
}

static void cmd_set(const char *args) {
  static char name[24], val[1300];
  const char *sp = strchr(args, ' ');
  const size_t nl = sp ? (size_t)(sp - args) : strlen(args);
  if (!nl || nl >= sizeof(name)) return (void)Serial.println("ERR ? usage: set KEY VALUE");
  memcpy(name, args, nl), name[nl] = 0;
  strlcpy(val, sp ? sp + 1 : "", sizeof(val));
  cfg_unescape(val);
  const CfgField *f = nullptr;
  for (const CfgField &c : CFG_FIELDS)
    if (!strcmp(c.name, name)) f = &c;
  if (!f) return (void)Serial.printf("ERR %s unknown setting\n", name);
  if (f->type == CF_STR && strlen(val) >= (size_t)f->size_or_min)
    return (void)Serial.printf("ERR %s too long (max %d)\n", name, f->size_or_min - 1);
  {
    CfgLock l;  // short: other tasks copy settings under it (the flash write below can take tens of ms)
    cfg_apply(*f, val);
  }
  cfg_save(*f);
  Serial.printf("OK %s\n", name);
}

// From the watch's own screens (the frame task): the value at once, in flash from loop() 0.6 s after the last change
// (cfg_ui_poll; a drag on a slider is one write, and a flash write never lands in the middle of a frame)
static volatile uint32_t cfg_ui_dirty;  // a bit per CFG_FIELDS index
static volatile uint32_t cfg_ui_at;

static void cfg_ui_set(const char *name, int v) {  // CF_INT or CF_BOOL
  for (int i = 0; i < CFG_N; i++) {
    const CfgField &f = CFG_FIELDS[i];
    if (strcmp(f.name, name)) continue;
    {
      CfgLock l;
      if (f.type == CF_BOOL) *(bool *)f.p = v != 0;
      else if (f.type == CF_INT) *(int *)f.p = constrain(v, f.size_or_min, f.max);
    }
    __atomic_or_fetch(&cfg_ui_dirty, 1u << i, __ATOMIC_RELAXED), cfg_ui_at = millis();
    return;
  }
}

static void cfg_ui_set_str(const char *name, const char *v) {  // CF_STR (not secrets)
  for (int i = 0; i < CFG_N; i++) {
    const CfgField &f = CFG_FIELDS[i];
    if (strcmp(f.name, name) || f.type != CF_STR) continue;
    {
      CfgLock l;
      strlcpy((char *)f.p, v, f.size_or_min);
    }
    __atomic_or_fetch(&cfg_ui_dirty, 1u << i, __ATOMIC_RELAXED), cfg_ui_at = millis();
    return;
  }
}

static void cfg_ui_poll(bool now = false) {  // from loop(); now — at once (before a restart)
  if (!cfg_ui_dirty || (!now && millis() - cfg_ui_at < 600)) return;
  const uint32_t d = __atomic_exchange_n(&cfg_ui_dirty, 0, __ATOMIC_RELAXED);
  for (int i = 0; i < CFG_N; i++)
    if (d & 1u << i) cfg_save(CFG_FIELDS[i]);
}

static void config_commands() {
  halo_command("cfg", [](const char *) {
    JBuf j;
    cfg_json(j);
    j.line("CFG");
  });
  halo_command("set", cmd_set);
  halo_command("reboot", [](const char *) {
    Serial.println("OK reboot");
    Serial.flush();
    delay(100);
    esp_restart();
  });
}
