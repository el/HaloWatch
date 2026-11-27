// gemini.h — the voice assistant's engine: Gemini Live (Google's speech-in, speech-out streaming API) over a
// WebSocket on TLS, written here on top of esp_tls (the frames are simple; reads never time out half-way through a
// frame, as they could with a generic transport, and a slow network only means waiting).
//
// The protocol (BidiGenerateContent, v1beta; field names — as in Google's SDK): first message "setup" — the model,
// AUDIO replies in the chosen voice, the system prompt, transcripts of both sides, the end of speech (by default the
// server detects it, and "audioStreamEnd" on release; with ai_auto_vad 0, "activityStart" / "activityEnd" mark the
// user's turn — gemini-3.1-flash-live-preview rejects those with 1007), Google Search grounding, session resumption
// (the conversation survives a reconnect) and a sliding context window. Then "realtimeInput.audio" — the microphone,
// 16-bit PCM at the I2S rate (24 kHz; the API resamples), base64, 100 ms per message. Replies: "serverContent" —
// "modelTurn.parts[].inlineData" (24 kHz PCM, base64), "outputTranscription" / "inputTranscription", "interrupted",
// "turnComplete"; "sessionResumptionUpdate", "goAway".
//
// Hold BOOT (assistant.h): the mic writes into `cap` (1 MB ring in PSRAM, ~21 s) at once — speaking can start before
// Wi-Fi and the session are up; the session task connects (net_acquire, TLS, setup), sends activityStart, streams
// what is in `cap`, and activityEnd once BOOT is released and `cap` is empty. Audio from the reply goes into `play`
// (another ring) and the speaker starts after 200 ms are buffered (a jittery network does not stutter). Pressing
// BOOT while it speaks — barge-in: the speaker stops at once and the rest of that reply is dropped. A press shorter
// than 0.3 s is not a question. The session stays open for ai_keep seconds after the last turn (default 120: a quick
// follow-up needs no reconnect), then closes; the next one resumes the conversation with the resumption handle.
// Commands: ai (state), ask TEXT (a typed question, the reply is spoken), mic test (records 3 s, plays them back, logs
// the level), ai close.
// Included from Oriole.ino after net.h and weather.h (the city for the prompt).
#include <esp_tls.h>
#include <mbedtls/base64.h>
#include <sys/socket.h>

#define AI_HOST "generativelanguage.googleapis.com"
#define AI_PATH "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"
#define AI_RATE 24000
#define AI_PREBUF (AI_RATE / 5)  // 200 ms in `play` before the speaker starts
#define AI_CHUNK (AI_RATE / 10)  // 100 ms per audio message
#define AI_RBUF (384 * 1024)     // the largest server message we take
#define AI_SBUF (16 * 1024)

enum : uint8_t { AI_IDLE, AI_CONNECTING, AI_LISTENING, AI_THINKING, AI_SPEAKING, AI_ERROR };
static const char *const AI_NAME[] = {"idle", "connecting", "listening", "thinking", "speaking", "error"};
static volatile uint8_t ai_state = AI_IDLE;
static char ai_error[120];
static volatile uint32_t ai_error_ms;

// ---------- Rings: one writer, one reader; w and r run freely, the size is a power of two ----------
struct AiRing {
  int16_t *b;
  uint32_t mask;
  volatile uint32_t w, r;
};
static AiRing cap, play;
static inline uint32_t ring_avail(const AiRing &q) {
  return q.w - q.r;
}
static inline uint32_t ring_space(const AiRing &q) {
  return q.mask + 1 - (q.w - q.r);
}
static void ring_put(AiRing &q, const int16_t *s, uint32_t n) {
  const uint32_t at = q.w & q.mask, first = min(n, q.mask + 1 - at);
  memcpy(q.b + at, s, first * 2), memcpy(q.b, s + first, (n - first) * 2);
  __sync_synchronize();
  q.w += n;
}
static void ring_get(AiRing &q, int16_t *d, uint32_t n) {
  const uint32_t at = q.r & q.mask, first = min(n, q.mask + 1 - at);
  memcpy(d, q.b + at, first * 2), memcpy(d + first, q.b, (n - first) * 2);
  __sync_synchronize();
  q.r += n;
}

// ---------- Shared with the screen (assistant.h) and the audio/mic tasks ----------
static volatile bool ai_hold;          // BOOT is held: the user is talking
static volatile uint32_t ai_press_ms;  // when it was pressed
static volatile bool ai_press_new;     // a new press for the session task
static volatile bool ai_short;         // released before 0.3 s: not a question
static volatile bool ai_capture;       // the mic writes into `cap`
static volatile bool ai_play_flush;    // the speaker drops what it has
static volatile bool ai_discard;       // drop the rest of the reply that was interrupted
static volatile bool ai_turn_done;     // the model finished its turn (play what is left, however little)
static volatile bool ai_playing;
static volatile float ai_in_level, ai_out_level;  // 0..1, for the orb
static volatile bool ai_cmd_close, ai_cmd_mictest;
static char ai_ask[400];
static volatile bool ai_ask_new;
// For the screen's finer states (assistant.h): waiting for Wi-Fi (not Gemini yet); a question being sent; how much of
// it was still to send when BOOT was let go (the "Sending… NN%" progress)
static volatile bool ai_net_wait, ai_in_turn;
static volatile uint32_t ai_send_total;
static TaskHandle_t ai_task_h;
// The session task's big buffers — in PSRAM (internal memory is for Wi-Fi and TLS)
struct AiScratch {
  int16_t chunk[AI_CHUNK];                // the mic, 100 ms
  char msg[AI_CHUNK * 8 / 3 + 128];       // its message
  unsigned char b64[4096];                // a reply's audio, base64 ...
  int16_t pcm[1536];                      // ... decoded
  char prompt[1600];                      // the system prompt with the time and the city
};
static AiScratch *ai_mem;
// Transcripts for the screen: what the user said and what the model says (the end of it, as live captions)
static portMUX_TYPE ai_mux = portMUX_INITIALIZER_UNLOCKED;
static char ai_you[240], ai_said[1200];
static volatile uint32_t ai_txt_ver;

static void ai_txt(char *dst, size_t cap_, const char *s, bool clear) {
  portENTER_CRITICAL(&ai_mux);
  if (clear) dst[0] = 0;
  size_t n = strlen(dst), k = strlen(s);
  if (n + k >= cap_) {  // too long: keep the end (from a space)
    size_t drop = n + k - cap_ + 1 + 64;
    if (drop > n) drop = n;
    while (drop < n && dst[drop] != ' ') drop++;
    memmove(dst, dst + drop, n - drop + 1), n -= drop;
    if (k >= cap_ - n) s += k - (cap_ - n - 1), k = strlen(s);
  }
  memcpy(dst + n, s, k + 1);
  ai_txt_ver++;
  portEXIT_CRITICAL(&ai_mux);
}

static void ai_fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ai_error, sizeof(ai_error), fmt, ap);
  va_end(ap);
  ai_error_ms = millis(), ai_state = AI_ERROR;
  LOG("assistant: %s\n", ai_error);
}

// The speaker (Oriole's audio task): the reply, once ai_prebuf is buffered or the turn is over. ai_prebuf starts at
// 200 ms and doubles (to 4 s) each time the reply runs dry mid-turn: on a slow link, a few short gaps, not a stutter
static volatile uint32_t ai_prebuf = AI_PREBUF;
static volatile uint32_t ai_gaps;  // the reply ran dry mid-turn (a gap is a click)
static volatile uint32_t ai_chime_until;  // the timer's alarm (apps.h): beeps instead of the reply until then (millis)
static volatile uint8_t ai_chime_mode;    // 0 — the alarm, 1 — a notification's ding (apps.h), 2 — a tone (games.h)
static volatile uint16_t ai_tone_hz = 440;  // mode 2's pitch

static int ai_chime(int16_t *out, int n) {  // two short 880 Hz beeps a second, the edges softened (no clicks)
  static uint32_t pos;  // samples since the chime started
  if ((int32_t)(millis() - ai_chime_until) >= 0) return pos = 0, 0;
  if (ai_chime_mode == 2) {  // a tone at ai_tone_hz until ai_chime_until, its start softened
    for (int i = 0; i < n; i++, pos++) {
      const float env = fminf(1, pos / 150.0f);
      out[i] = (int16_t)(7000 * env * sinf(2 * ORIOLE_PI * ai_tone_hz * (pos % AI_RATE) / AI_RATE));
    }
    return n;
  }
  if (ai_chime_mode == 1) {  // the ding: 1320 Hz, then 1760 Hz, 90 ms each
    for (int i = 0; i < n; i++, pos++) {
      const uint32_t t = pos, len = AI_RATE * 9 / 100, gap = AI_RATE * 11 / 100;
      const bool second = t >= gap && t < gap + len;
      const uint32_t in = second ? t - gap : t;
      const float f = t < len ? 1320 : second ? 1760 : 0;
      const float env = f ? fminf(1, fminf(in, len - in) / 100.0f) : 0;
      out[i] = (int16_t)(6000 * env * sinf(2 * ORIOLE_PI * f * in / AI_RATE));
    }
    return n;
  }
  for (int i = 0; i < n; i++, pos++) {
    const uint32_t t = pos % AI_RATE;  // in the second: beep 0..150 ms, beep 250..400 ms
    const bool on = t < AI_RATE * 15 / 100 || (t >= AI_RATE / 4 && t < AI_RATE * 40 / 100);
    const uint32_t in = t < AI_RATE / 4 ? t : t - AI_RATE / 4, len = AI_RATE * 15 / 100;
    const float env = on ? fminf(1, fminf(in, len - in) / 120.0f) : 0;
    out[i] = (int16_t)(9000 * env * sinf(2 * ORIOLE_PI * 880 * t / AI_RATE));  // whole cycles a second: t is enough
  }
  return n;
}
static int ai_fill(int16_t *out, int n) {
  if (const int c = ai_chime(out, n)) return c;
  if (ai_play_flush) play.r = play.w, ai_play_flush = false, ai_playing = false;
  const uint32_t a = ring_avail(play);
  if (!ai_playing) {
    if (a >= ai_prebuf || (ai_turn_done && a > 0)) ai_playing = true;
    else return ai_out_level = 0, 0;
  }
  const int k = min((uint32_t)n, a);
  ring_get(play, out, k);
  if (k < n) {  // ran dry: wait for more (or the end of the turn)
    ai_playing = false;
    if (!ai_turn_done) ai_prebuf = min<uint32_t>(ai_prebuf * 2, AI_RATE * 4), ai_gaps++;
  }
  float e = 0;
  for (int i = 0; i < k; i++) e += (float)out[i] * out[i];
  ai_out_level = k ? sqrtf(e / k) / 8000 : 0;
  return k;
}

// The microphone (Oriole's mic task): DC blocked, into `cap` while capturing
static void ai_mic(const int16_t *b, int n) {
  static float x1, y1;
  int16_t t[256];
  float e = 0;
  n = min(n, 256);
  for (int i = 0; i < n; i++) {
    const float y = b[i] - x1 + 0.995f * y1;
    x1 = b[i], y1 = y;
    t[i] = (int16_t)constrain(y, -32768.0f, 32767.0f), e += y * y;
  }
  ai_in_level = sqrtf(e / n) / 3000;
  if (ai_capture && ring_space(cap) >= (uint32_t)n) ring_put(cap, t, n);
}

// ---------- WebSocket over TLS ----------
static esp_tls_t *ws_tls;
static uint8_t *ws_sbuf;
static char *ws_rbuf;
static char ws_close_reason[100];

static bool ws_write_all(const void *d, size_t n) {
  const uint8_t *p = (const uint8_t *)d;
  const uint32_t t0 = millis();
  while (n) {
    const int r = esp_tls_conn_write(ws_tls, p, n);
    if (r > 0) p += r, n -= r;
    else if ((r != ESP_TLS_ERR_SSL_WANT_READ && r != ESP_TLS_ERR_SSL_WANT_WRITE) || millis() - t0 > 10000) return false;
  }
  return true;
}

static bool ws_read_all(void *d, size_t n) {
  uint8_t *p = (uint8_t *)d;
  const uint32_t t0 = millis();
  while (n) {
    const int r = esp_tls_conn_read(ws_tls, p, n);
    if (r > 0) p += r, n -= r;
    else if (r == 0 || (r != ESP_TLS_ERR_SSL_WANT_READ && r != ESP_TLS_ERR_SSL_WANT_WRITE) || millis() - t0 > 15000)
      return false;
  }
  return true;
}

static bool ws_send(uint8_t op, const void *payload, size_t n) {  // one masked frame (client frames are masked)
  if (n > AI_SBUF - 14) return false;
  const int ext = n < 126 ? 0 : n < 65536 ? 2 : 8, h = 2 + ext + 4;
  uint8_t *f = ws_sbuf + 14 - h;
  f[0] = 0x80 | op, f[1] = 0x80 | (ext == 0 ? n : ext == 2 ? 126 : 127);
  if (ext == 2) f[2] = n >> 8, f[3] = n;
  if (ext == 8) memset(f + 2, 0, 4), f[6] = n >> 24, f[7] = n >> 16, f[8] = n >> 8, f[9] = n;
  uint8_t *mk = f + 2 + ext;
  esp_fill_random(mk, 4);
  uint8_t *p = ws_sbuf + 14;
  memcpy(p, payload, n);
  for (size_t i = 0; i < n; i++) p[i] ^= mk[i & 3];
  return ws_write_all(f, h + n);
}

static bool ws_pending(int ms) {  // a frame is waiting (decrypted bytes or bytes on the socket)
  if (esp_tls_get_bytes_avail(ws_tls) > 0) return true;
  int fd = -1;
  if (esp_tls_get_conn_sockfd(ws_tls, &fd) != ESP_OK || fd < 0) return false;
  fd_set rs;
  FD_ZERO(&rs);
  FD_SET(fd, &rs);
  struct timeval tv = {0, ms * 1000};
  return select(fd + 1, &rs, nullptr, nullptr, &tv) > 0;
}

// One whole message (text or binary, continuation frames joined) into ws_rbuf, NUL-terminated: its length; 0 — a
// control frame or a message too large (skipped); −1 — closed or broken (ws_close_reason says why, if the server did)
static int ws_recv() {
  size_t got = 0;
  bool over = false;
  for (;;) {
    uint8_t h[2];
    if (!ws_read_all(h, 2)) return -1;
    const bool fin = h[0] & 0x80;
    const uint8_t op = h[0] & 15;
    uint64_t len = h[1] & 127;
    if (len == 126) {
      uint8_t e[2];
      if (!ws_read_all(e, 2)) return -1;
      len = e[0] << 8 | e[1];
    } else if (len == 127) {
      uint8_t e[8];
      if (!ws_read_all(e, 8)) return -1;
      len = 0;
      for (int i = 0; i < 8; i++) len = len << 8 | e[i];
    }
    uint8_t mk[4] = {0, 0, 0, 0};
    if ((h[1] & 0x80) && !ws_read_all(mk, 4)) return -1;
    if (op >= 8) {  // control: ping -> pong, close -> why
      uint8_t c[126];
      if (len > 125 || !ws_read_all(c, len)) return -1;
      for (size_t i = 0; i < len; i++) c[i] ^= mk[i & 3];
      if (op == 9) ws_send(10, c, len);
      if (op == 8) {
        const int code = len >= 2 ? c[0] << 8 | c[1] : 0;
        snprintf(ws_close_reason, sizeof(ws_close_reason), "%d %.*s", code, len > 2 ? (int)len - 2 : 0, (char *)c + 2);
        return -1;
      }
      if (fin && !got) return 0;
      continue;
    }
    if (over || got + len >= AI_RBUF) {
      over = true;
      uint8_t junk[256];
      for (uint64_t left = len; left;) {
        const size_t k = min<uint64_t>(left, sizeof(junk));
        if (!ws_read_all(junk, k)) return -1;
        left -= k;
      }
    } else {
      if (!ws_read_all(ws_rbuf + got, len)) return -1;
      got += len;
    }
    if (fin) break;
  }
  if (over) return LOG("assistant: a message too large was skipped\n"), 0;
  ws_rbuf[got] = 0;
  return got;
}

static void ws_close() {
  if (!ws_tls) return;
  const uint8_t bye[2] = {0x03, 0xE8};  // 1000: normal
  ws_send(8, bye, 2);
  esp_tls_conn_destroy(ws_tls);
  ws_tls = nullptr;
}

static bool ws_open(const char *key) {
  ws_close_reason[0] = 0;
  esp_tls_cfg_t c = {};
  c.crt_bundle_attach = esp_crt_bundle_attach, c.timeout_ms = 10000;
  ws_tls = esp_tls_init();
  if (!ws_tls) return ai_fail("Out of memory (TLS)"), false;
  if (esp_tls_conn_new_sync(AI_HOST, strlen(AI_HOST), 443, &c, ws_tls) != 1) {
    esp_tls_conn_destroy(ws_tls), ws_tls = nullptr;
    return ai_fail("Could not reach Gemini (TLS)"), false;
  }
  uint8_t rnd[16];
  unsigned char k64[32];
  size_t kl = 0;
  esp_fill_random(rnd, 16);
  mbedtls_base64_encode(k64, sizeof(k64), &kl, rnd, 16);
  k64[kl] = 0;
  char req[512];
  const int n = snprintf(req, sizeof(req),
                         "GET " AI_PATH " HTTP/1.1\r\nHost: " AI_HOST "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nx-goog-api-key: %s\r\n"
                         "User-Agent: Oriole\r\n\r\n",
                         k64, key);
  if (n >= (int)sizeof(req) || !ws_write_all(req, n)) return ws_close(), ai_fail("Could not send the request"), false;
  char resp[1024];
  int m = 0;  // the response's head, a byte at a time (nothing of the first frame is read with it)
  while (m < (int)sizeof(resp) - 1) {
    if (!ws_read_all(resp + m, 1)) break;
    m++;
    if (m >= 4 && !memcmp(resp + m - 4, "\r\n\r\n", 4)) break;
  }
  resp[m] = 0;
  const int code = m > 12 ? atoi(resp + 9) : 0;
  if (code != 101) {
    esp_tls_conn_destroy(ws_tls), ws_tls = nullptr;
    return ai_fail(code == 400 || code == 403 ? "API key rejected (HTTP %d)" : "Gemini: HTTP %d", code), false;
  }
  return true;
}

// ---------- The session ----------
static char ai_handle[256];          // session resumption: the conversation survives a reconnect
static bool ai_setup_ok, ai_goaway, ai_model_turn, ai_got_audio;
static uint32_t ai_last_ms;

static void ai_audio_b64(const char *v) {  // base64 PCM from a string value, into `play`
  unsigned char *q = ai_mem->b64;
  int16_t *pcm = ai_mem->pcm;
  if (!v || *v != '"') return;
  v++;
  for (;;) {
    int k = 0;
    while (k < (int)sizeof(ai_mem->b64) && *v && *v != '"') {
      if (*v != '\\') q[k++] = *v;
      v++;
    }
    const bool end = !*v || *v == '"';  // not the end: k is 4096, whole quartets
    size_t got = 0;
    if (k && mbedtls_base64_decode((unsigned char *)pcm, sizeof(ai_mem->pcm), &got, q, k) != 0) return;
    for (uint32_t at = 0, n = got / 2; at < n && !ai_discard;) {  // the speaker drains it; drop if barged in
      const uint32_t m = min(n - at, ring_space(play));
      if (!m) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      ring_put(play, pcm + at, m), at += m;
    }
    if (end) return;
  }
}

static void ai_on_message(char *m) {
  if (strstr(m, "\"setupComplete\"")) ai_setup_ok = true;
  if (char *u = strstr(m, "\"sessionResumptionUpdate\"")) {
    if (js_true(u, "resumable")) js_str(js_find(u, "newHandle"), ai_handle, sizeof(ai_handle));
  }
  if (strstr(m, "\"goAway\"")) ai_goaway = true, LOG("assistant: the server will close the session soon\n");
  char *sc = strstr(m, "\"serverContent\"");
  if (!sc) return;
  ai_last_ms = millis();
  if (js_true(sc, "interrupted")) {
    ai_discard = false, ai_model_turn = false;
    if (!ai_hold) ai_play_flush = true;
  }
  char t[400];
  if (char *p = strstr(sc, "\"inputTranscription\"")) {
    js_str(js_find(p, "text"), t, sizeof(t));
    if (*t) ai_txt(ai_you, sizeof(ai_you), t, false);
  }
  for (char *p = sc; (p = strstr(p, "\"inlineData\"")); p++) {
    if (!ai_model_turn) ai_model_turn = true, ai_turn_done = false;
    if (ai_discard) continue;
    if (!ai_got_audio) ai_got_audio = true, ai_state = AI_SPEAKING;
    ai_audio_b64(js_find(p, "data"));
  }
  if (char *p = strstr(sc, "\"outputTranscription\"")) {
    js_str(js_find(p, "text"), t, sizeof(t));
    if (*t && !ai_discard) {
      if (!ai_model_turn) ai_model_turn = true, ai_turn_done = false;
      ai_txt(ai_said, sizeof(ai_said), t, false);
    }
  }
  if (js_true(sc, "turnComplete")) {
    ai_model_turn = false;
    if (ai_discard) ai_discard = false;  // the interrupted reply is over; what comes next is the new one
    else ai_turn_done = true, hist_push(ai_you, ai_said);  // history.h (this task writes both strings)
  }
}

static void ai_setup_json(JBuf &j, bool resume) {
  char model[96], voice[32], lang[16];
  bool search, auto_vad;
  char *prompt = ai_mem->prompt;
  const size_t PROMPT = sizeof(ai_mem->prompt);
  {
    CfgLock l;
    snprintf(model, sizeof(model), "%s%s", strncmp(cfg.model, "models/", 7) ? "models/" : "", cfg.model);
    strlcpy(voice, cfg.voice, sizeof(voice)), strlcpy(lang, cfg.lang, sizeof(lang));
    strlcpy(prompt, cfg.prompt, PROMPT);
    search = cfg.search, auto_vad = cfg.auto_vad;
  }
  // What the watch knows: the local time and the user's city (the first weather city)
  if (oriole_clock_valid()) {
    const time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[64];
    strftime(ts, sizeof(ts), "%A, %B %d, %Y, %I:%M %p", &tm);
    const size_t n = strlen(prompt);
    snprintf(prompt + n, PROMPT - n, "\n\nThe user's local time now: %s.", ts);
  }
  if (wx_n > 0 && wx_city_name[0][0]) {
    const size_t n = strlen(prompt);
    snprintf(prompt + n, PROMPT - n, " The user is in or near %s.", wx_city_name[0]);
  }
  j.put("{\"setup\":{");
  j.key("model"), j.str(model);
  j.put(",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],\"speechConfig\":{");
  j.put("\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":"), j.str(*voice ? voice : "Puck"), j.put("}}");
  if (*lang) j.key("languageCode"), j.str(lang);
  j.put("}}");
  j.put(",\"systemInstruction\":{\"parts\":[{\"text\":"), j.str(prompt), j.put("}]}");
  if (!auto_vad) j.put(",\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":true}}");
  j.put(",\"inputAudioTranscription\":{},\"outputAudioTranscription\":{}");
  j.put(",\"contextWindowCompression\":{\"slidingWindow\":{}}");
  if (resume && *ai_handle) j.put(",\"sessionResumption\":{\"handle\":"), j.str(ai_handle), j.put("}");
  else j.put(",\"sessionResumption\":{}");
  if (search) j.put(",\"tools\":[{\"googleSearch\":{}}]");
  j.put("}}");
}

static bool ai_connect() {
  char key[128];
  {
    CfgLock l;
    strlcpy(key, cfg.key, sizeof(key));
  }
  if (!*key) return ai_fail("Add a Gemini API key in the web controller"), false;
  for (int attempt = 0; attempt < 2; attempt++) {
    const bool resume = attempt == 0 && *ai_handle;
    if (attempt && !*ai_handle) break;
    if (attempt) ai_handle[0] = 0, LOG("assistant: starting a new conversation\n");
    xSemaphoreTake(net_tls_mx, portMAX_DELAY);
    const uint32_t t0 = millis();
    if (!ws_open(key)) return xSemaphoreGive(net_tls_mx), false;
    JBuf j;
    ai_setup_json(j, resume);
    ai_setup_ok = ai_goaway = false;
    bool ok = j.p && ws_send(1, j.p, j.n);
    while (ok && !ai_setup_ok && millis() - t0 < 12000) {
      if (!ws_pending(100)) continue;
      const int n = ws_recv();
      if (n < 0) ok = false;
      else if (n > 0) ai_on_message(ws_rbuf);
    }
    if (ok && ai_setup_ok) {
      ai_prebuf = AI_PREBUF;  // a new connection: the link may be better now
      LOG("assistant: session %s in %lu ms\n", resume ? "resumed" : "open", (unsigned long)(millis() - t0));
      return true;
    }
    LOG("assistant: setup failed: %s\n", ws_close_reason[0] ? ws_close_reason : "no answer");
    if (ws_tls) esp_tls_conn_destroy(ws_tls), ws_tls = nullptr;
    xSemaphoreGive(net_tls_mx);
    if (!resume) break;  // a resumed session failed — try once more as a new one
  }
  char why[100];
  strlcpy(why, ws_close_reason[0] ? ws_close_reason : "no answer", sizeof(why));
  if (strstr(why, "API key") || strstr(why, "PERMISSION") || strstr(why, "1008")) ai_fail("Gemini: %.90s", why);
  else ai_fail("Gemini setup failed: %.80s", why);
  return false;
}

static void ai_disconnect() {
  ws_close();
  xSemaphoreGive(net_tls_mx);
  net_release();
}

static bool ai_send_json(const char *s) {
  return ws_send(1, s, strlen(s));
}

static bool ai_send_audio(const int16_t *pcm, int n) {
  static const char HEAD[] = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"";
  char *msg = ai_mem->msg;
  const size_t hl = sizeof(HEAD) - 1;
  memcpy(msg, HEAD, hl);
  size_t bl = 0;
  mbedtls_base64_encode((unsigned char *)msg + hl, sizeof(ai_mem->msg) - hl - 8, &bl, (const unsigned char *)pcm, n * 2);
  memcpy(msg + hl + bl, "\"}}}", 4);
  return ws_send(1, msg, hl + bl + 4);
}

static bool ai_send_text(const char *text, bool manual) {
  JBuf j;
  j.put("{\"realtimeInput\":{\"text\":"), j.str(text), j.put("}}");
  return (!manual || ai_send_json("{\"realtimeInput\":{\"activityStart\":{}}}")) && j.p && ws_send(1, j.p, j.n) &&
         (!manual || ai_send_json("{\"realtimeInput\":{\"activityEnd\":{}}}"));
}

static void ai_mic_test() {  // 3 s from the mic, then played back; the level in the log
  LOG("mic test: speak now (3 s)\n");
  cap.r = cap.w;
  if (!oriole_mic_on(ai_mic)) return ai_fail("No microphone (ES7210)");
  ai_capture = true;
  float peak = 0;
  for (uint32_t t0 = millis(); millis() - t0 < 3000;) vTaskDelay(pdMS_TO_TICKS(20)), peak = fmaxf(peak, ai_in_level);
  ai_capture = false;
  oriole_mic_off();
  int16_t *tmp = ai_mem->chunk;
  int64_t e = 0, n = 0;
  int pk = 0;
  ai_turn_done = true;
  while (uint32_t a = ring_avail(cap)) {
    const uint32_t k = min<uint32_t>(a, AI_CHUNK);
    ring_get(cap, tmp, k);
    for (uint32_t i = 0; i < k; i++) e += (int64_t)tmp[i] * tmp[i], pk = max(pk, abs((int)tmp[i]));
    n += k;
    while (ring_space(play) < k) vTaskDelay(pdMS_TO_TICKS(10));
    ring_put(play, tmp, k);
  }
  const float rms = n ? sqrtf((float)e / n) : 0;
  LOG("mic test: %.1f s, rms %.0f (%.1f dBFS), peak %d — playing it back\n", n / (float)AI_RATE, rms,
      rms > 0 ? 20 * log10f(rms / 32768) : -99.0f, pk);
  ai_state = AI_SPEAKING;
}

static void ai_task(void *) {
  oriole_audio_begin(ai_fill, AI_RATE);
  oriole_audio_volume(cfg.vol);
  oriole_mic_gain(cfg.gain), oriole_mic_channel(cfg.mic_ch);
  bool conn = false, in_turn = false;
  uint32_t think_ms = 0;
  int16_t *chunk = ai_mem->chunk;
  uint8_t logged_state = ai_state;
  for (;;) {
    if (ai_state != logged_state) logged_state = ai_state, LOG("assistant: %s (speaker buffer %lu ms, gaps %lu, underruns %lu)\n", AI_NAME[ai_state],
          (unsigned long)(ai_prebuf * 1000 / AI_RATE), (unsigned long)ai_gaps, (unsigned long)oriole_audio_underruns());
    const bool manual = !cfg.auto_vad;
    // A short tap of BOOT: not a question
    if (ai_press_new && !ai_hold && ai_short) ai_press_new = false, cap.r = cap.w;
    const bool want = ai_hold || ai_press_new || ring_avail(cap) > 0 || ai_ask_new;
    if (ai_cmd_mictest && !ai_hold && !in_turn) ai_cmd_mictest = false, ai_mic_test();
    if (ai_state == AI_ERROR && millis() - ai_error_ms > 6000) ai_state = AI_IDLE;
    if (!conn) {
      ai_cmd_close = false;  // nothing to close (a setting changed: the next session uses it anyway)
      if (ai_state == AI_SPEAKING && !ai_playing && !ring_avail(play)) ai_state = AI_IDLE;
      if (!want) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        continue;
      }
      ai_state = AI_CONNECTING;
      ai_net_wait = true;
      const bool net_ok = net_acquire(25000);
      ai_net_wait = false;
      if (!net_ok) {
        ai_fail(net_state == NET_NO_SSID ? "Set up Wi-Fi in the web controller" : "No Wi-Fi connection");
      } else if (!ai_connect()) {
        net_release();
      } else {
        conn = true, ai_last_ms = millis(), ai_state = ai_hold ? AI_LISTENING : AI_IDLE;
      }
      if (!conn) {  // drop what was said; wait for the next press
        ai_press_new = ai_ask_new = false, cap.r = cap.w;
        while (ai_hold) vTaskDelay(pdMS_TO_TICKS(20)), cap.r = cap.w;
        continue;
      }
    }
    bool ok = true;
    // A new press: the user's turn starts (after 0.3 s held, or on release if it was long enough)
    if (ai_press_new && (!ai_hold || millis() - ai_press_ms >= 300)) {
      ai_press_new = false;
      if (ai_model_turn) ai_discard = true;  // barge-in: the rest of that reply is dropped
      ai_play_flush = true, ai_turn_done = false, ai_got_audio = false;
      ai_txt(ai_you, sizeof(ai_you), "", true), ai_txt(ai_said, sizeof(ai_said), "", true);
      if (manual) ok = ai_send_json("{\"realtimeInput\":{\"activityStart\":{}}}");
      in_turn = ai_in_turn = true, ai_state = AI_LISTENING;
    }
    // Mic audio that belongs to no turn (the connection dropped while talking): dropped
    if (!in_turn && !ai_press_new && ring_avail(cap)) cap.r = cap.w;
    // The microphone, 100 ms at a time (and the rest once released)
    while (ok && in_turn && (ring_avail(cap) >= AI_CHUNK || (!ai_hold && ring_avail(cap) > 0))) {
      const uint32_t k = min<uint32_t>(ring_avail(cap), AI_CHUNK);
      ring_get(cap, chunk, k);
      ok = ai_send_audio(chunk, k), ai_last_ms = millis();
    }
    if (ok && in_turn && !ai_hold && !ring_avail(cap)) {
      ok = ai_send_json(manual ? "{\"realtimeInput\":{\"activityEnd\":{}}}" : "{\"realtimeInput\":{\"audioStreamEnd\":true}}");
      in_turn = ai_in_turn = false, ai_state = AI_THINKING, think_ms = millis();
    }
    if (ok && ai_ask_new && !in_turn) {  // a typed question (`ask`)
      ai_ask_new = false, ai_play_flush = true, ai_turn_done = false, ai_got_audio = false;
      ai_txt(ai_you, sizeof(ai_you), ai_ask, true), ai_txt(ai_said, sizeof(ai_said), "", true);
      ok = ai_send_text(ai_ask, manual), ai_state = AI_THINKING, think_ms = millis(), ai_last_ms = millis();
    }
    // Whatever the server sent
    for (int i = 0; ok && i < 8 && ws_pending(i ? 0 : 10); i++) {
      const int n = ws_recv();
      if (n < 0) ok = false;
      else if (n > 0) ai_on_message(ws_rbuf);
    }
    // The state for the screen
    if (ai_state == AI_THINKING && ai_got_audio) ai_state = AI_SPEAKING;
    if (ai_state == AI_THINKING && millis() - think_ms > 45000) ai_fail("No answer from Gemini");
    if (ai_state == AI_SPEAKING && ai_turn_done && !ai_playing && !ring_avail(play)) ai_state = AI_IDLE;
    if (!ok) {
      LOG("assistant: connection closed%s%s\n", ws_close_reason[0] ? ": " : "", ws_close_reason);
      if (in_turn || ai_state == AI_THINKING || (ai_state == AI_SPEAKING && !ai_turn_done))
        ai_fail("Connection lost%s%.60s", ws_close_reason[0] ? ": " : "", ws_close_reason);
      ai_disconnect(), conn = in_turn = ai_in_turn = ai_model_turn = false;
      ai_turn_done = true;
      continue;
    }
    // Idle long enough (or the server is going away, or `ai close`): close; the next press resumes
    const bool quiet = !in_turn && !ai_hold && !ai_press_new && (ai_state == AI_IDLE || ai_state == AI_ERROR);
    if (quiet && (ai_cmd_close || ai_goaway || millis() - ai_last_ms > (uint32_t)cfg.keep_s * 1000)) {
      LOG("assistant: session closed (%s)\n", ai_cmd_close ? "command" : ai_goaway ? "server" : "idle");
      ai_cmd_close = false;
      ai_disconnect(), conn = false;
    }
  }
}

static bool ai_busy() {  // the assistant is doing something: keep the screen on, the board up
  return ai_hold || ai_press_new || (int32_t)(millis() - ai_chime_until) < 0 || ai_state == AI_CONNECTING ||
         ai_state == AI_LISTENING ||
         ai_state == AI_THINKING || ai_state == AI_SPEAKING || ai_playing;
}

static void ai_begin() {
  cap.mask = play.mask = (1 << 19) - 1;  // 524 288 samples each: ~21.8 s at 24 kHz
  cap.b = (int16_t *)heap_caps_malloc((cap.mask + 1) * 2, MALLOC_CAP_SPIRAM);
  play.b = (int16_t *)heap_caps_malloc((play.mask + 1) * 2, MALLOC_CAP_SPIRAM);
  ws_sbuf = (uint8_t *)heap_caps_malloc(AI_SBUF, MALLOC_CAP_SPIRAM);
  ws_rbuf = (char *)heap_caps_malloc(AI_RBUF, MALLOC_CAP_SPIRAM);
  ai_mem = (AiScratch *)heap_caps_malloc(sizeof(AiScratch), MALLOC_CAP_SPIRAM);
  if (!cap.b || !play.b || !ws_sbuf || !ws_rbuf || !ai_mem) return (void)LOG("assistant: NO MEMORY\n");
  // Its stack in PSRAM (it never writes flash, the only thing a PSRAM stack must not do): internal RAM is short
  xTaskCreatePinnedToCoreWithCaps(ai_task, "assistant", 10240, nullptr, 4, &ai_task_h, 0, MALLOC_CAP_SPIRAM);
  oriole_command("ai", [](const char *a) {
    if (!strcmp(a, "close")) ai_cmd_close = true;
    LOG("assistant: %s%s%s\n", AI_NAME[ai_state], ai_state == AI_ERROR ? " — " : "", ai_state == AI_ERROR ? ai_error : "");
  });
  oriole_command("ask", [](const char *a) {
    if (!*a) return (void)Serial.println("ERR ask usage: ask TEXT");
    strlcpy(ai_ask, a, sizeof(ai_ask)), ai_ask_new = true;
    xTaskNotifyGive(ai_task_h);
    Serial.println("OK ask");
  });
  oriole_command("mic", [](const char *a) {
    if (strcmp(a, "test")) return (void)Serial.println("ERR mic usage: mic test");
    ai_cmd_mictest = true;
    xTaskNotifyGive(ai_task_h);
    Serial.println("OK mic");
  });
}
