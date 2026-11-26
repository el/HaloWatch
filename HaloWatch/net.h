// net.h — Wi-Fi on demand, the clock from the internet, HTTPS requests.
//
// Wi-Fi is on only while something needs it: the assistant's session, a weather update, a time sync — and for
// net_idle seconds after the last use (default 60), then the radio goes off. While it is on, the board does not really
// sleep (halo_stay_awake: in light sleep the access point would drop us); Always On still dims the screen as usual.
// Users: net_acquire() (blocks until connected or failed; not from the frame task) and net_release(); net_poke()
// starts connecting without waiting (the frame task, when BOOT is pressed: the radio is warming up while you talk).
//
// The clock: SNTP (pool.ntp.org, time.google.com) whenever Wi-Fi is up, and the `time` command (the web controller
// sends the computer's time when it connects). Until one of them sets it, the face shows "--:--". The time zone:
// the `tz` setting (POSIX, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"), or, when it is empty, "auto": the UTC offset of the
// first weather city (Open-Meteo reports it with the forecast), or the computer's offset from `time`.
// Included from HaloWatch.ino after config.h.
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_sntp.h>
#include <mbedtls/platform.h>

// TLS buffers in PSRAM: the core allocates them from internal RAM only, and ~45 KB per connection is more than this
// sketch leaves free there (mbedtls_ssl_setup: -0x7F00, every HTTPS request failed). Internal RAM is the fallback.
static void *net_tls_calloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// ---------- JSON: just enough to read the replies of Gemini and Open-Meteo (keys found by name, whitespace allowed) ----------
static char *js_find(char *from, const char *key) {  // the value after "key":, or nullptr
  const size_t kl = strlen(key);
  for (char *p = from; (p = strchr(p, '"'));) {
    if (!strncmp(p + 1, key, kl) && p[kl + 1] == '"') {
      char *v = p + kl + 2;
      while (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t') v++;
      if (*v == ':') {
        v++;
        while (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t') v++;
        return v;
      }
    }
    p++;
  }
  return nullptr;
}

static bool js_true(char *from, const char *key) {
  const char *v = js_find(from, key);
  return v && !strncmp(v, "true", 4);
}

static size_t js_str(const char *v, char *out, size_t cap_) {  // a string value, unescaped, UTF-8
  size_t n = 0;
  if (!v || *v != '"') return out[0] = 0, 0;
  auto put = [&](uint32_t c) {
    char u[4];
    int k = 0;
    if (c < 0x80) u[k++] = c;
    else if (c < 0x800) u[k++] = 0xC0 | c >> 6, u[k++] = 0x80 | (c & 63);
    else u[k++] = 0xE0 | c >> 12, u[k++] = 0x80 | (c >> 6 & 63), u[k++] = 0x80 | (c & 63);
    if (n + k < cap_) memcpy(out + n, u, k), n += k;
  };
  for (v++; *v && *v != '"'; v++) {
    if (*v != '\\') {
      if (n + 1 < cap_) out[n++] = *v;
      continue;
    }
    v++;
    switch (*v) {
      case 'n': put('\n'); break;
      case 't': put('\t'); break;
      case 'u': {
        char hx[5] = {0};
        for (int i = 0; i < 4 && v[1]; i++) hx[i] = *++v;
        put(strtoul(hx, nullptr, 16));
        break;
      }
      case 0: v--; break;
      default: put(*v);
    }
  }
  out[n] = 0;
  return n;
}

enum : uint8_t { NET_OFF, NET_CONNECTING, NET_UP, NET_FAILED, NET_NO_SSID };
static const char *const NET_NAME[] = {"off", "connecting", "connected", "failed", "no_ssid"};
static volatile uint8_t net_state = NET_OFF;
static volatile int net_users;
static volatile bool net_poked, net_reconnect;
static volatile uint32_t net_last_ms;  // the last use (millis)
static volatile bool net_time_ok;      // the clock has been set (SNTP or `time`)
static volatile int32_t net_auto_off = INT32_MIN;  // the "auto" time zone's UTC offset, s (INT32_MIN — not known)
static bool net_sntp_started;
static TaskHandle_t net_task_h;
static SemaphoreHandle_t net_tls_mx;  // one TLS connection at a time (a binary semaphore: net_begin says why)

static void bt_radio(bool wifi);  // ancs.h: Bluetooth gets the radio (and its 72 KB) back whenever Wi-Fi is off

static bool halo_clock_valid() {
  return net_time_ok;
}

static void net_apply_tz() {  // the `tz` setting, or the auto offset, or UTC
  char tz[64];
  {
    CfgLock l;
    strlcpy(tz, cfg.tz, sizeof(tz));
  }
  if (!*tz) {
    const int32_t off = net_auto_off == INT32_MIN ? 0 : net_auto_off;
    const int32_t a = abs(off) / 60;  // POSIX: the sign is the other way round ("UTC-2" is two hours east)
    snprintf(tz, sizeof(tz), "UTC%c%ld:%02ld", off > 0 ? '-' : '+', (long)(a / 60), (long)(a % 60));
  }
  setenv("TZ", tz, 1), tzset();
}

static void net_set_auto_offset(int32_t off_s) {  // from the weather (Open-Meteo's utc_offset_seconds)
  if (off_s == net_auto_off) return;
  net_auto_off = off_s;
  if (!cfg.tz[0]) net_apply_tz(), LOG("time zone: auto, UTC%+.1f h\n", off_s / 3600.0f);
}

static void net_time_set(time_t t, const char *how) {
  const struct timeval tv = {t, 0};
  settimeofday(&tv, nullptr);
  if (!net_time_ok) LOG("clock set (%s)\n", how);
  net_time_ok = true;
}

static void net_task(void *) {
  uint32_t since = 0, retry_at = 0;
  bool held = false;
  char ssid[33], pass[65];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    const uint32_t now = millis();
    if (net_reconnect && (net_state == NET_UP || net_state == NET_CONNECTING)) {  // new Wi-Fi settings: start over
      LOG("wifi: reconnecting with the new settings\n");
      WiFi.disconnect(true), WiFi.mode(WIFI_OFF);
      net_state = NET_OFF;
    }
    net_reconnect = false;
    const bool want = net_users > 0 || net_poked || now - net_last_ms < (uint32_t)cfg.net_idle * 1000;
    switch (net_state) {
      case NET_OFF:
      case NET_FAILED:
      case NET_NO_SSID:
        if (!want) {
          if (net_state != NET_OFF) net_state = NET_OFF;
          break;
        }
        if (net_state == NET_FAILED && (int32_t)(now - retry_at) < 0 && !net_poked) break;
        {
          CfgLock l;
          strlcpy(ssid, cfg.ssid, sizeof(ssid)), strlcpy(pass, cfg.pass, sizeof(pass));
        }
        net_poked = false;
        if (!*ssid) {
          net_state = NET_NO_SSID;
          break;
        }
        if (!held) halo_stay_awake(true), held = true;
        bt_radio(true);  // Bluetooth stops first: the two do not fit in internal RAM together
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);  // streaming audio: no modem sleep (the radio is on only while it is used anyway)
        WiFi.begin(ssid, pass);
        LOG("wifi: connecting to %s\n", ssid);
        net_state = NET_CONNECTING, since = now;
        break;
      case NET_CONNECTING:
        if (WiFi.status() == WL_CONNECTED) {
          net_state = NET_UP;
          LOG("wifi: connected, %s, %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
          if (!net_sntp_started) {
            net_sntp_started = true;
            sntp_set_time_sync_notification_cb([](struct timeval *) { net_time_ok = true, LOG("clock synced (SNTP)\n"); });
            configTime(0, 0, "pool.ntp.org", "time.google.com");
            net_apply_tz();  // configTime resets TZ
          }
        } else if (now - since > 15000) {
          LOG("wifi: could not connect to %s (status %d)\n", ssid, (int)WiFi.status());
          WiFi.disconnect(true), WiFi.mode(WIFI_OFF);
          net_state = NET_FAILED, retry_at = now + 60000;
        }
        break;
      case NET_UP:
        net_poked = false;  // a poke while up keeps it up through net_last_ms; left set, Wi-Fi never went idle
        if (WiFi.status() != WL_CONNECTED) {
          LOG("wifi: lost the connection\n");
          WiFi.disconnect(true), WiFi.mode(WIFI_OFF);
          net_state = NET_FAILED, retry_at = now + 5000;
        } else if (!want) {
          LOG("wifi: off (idle)\n");
          WiFi.disconnect(true), WiFi.mode(WIFI_OFF);
          net_state = NET_OFF;
        }
        break;
    }
    const bool radio_off = net_state == NET_OFF || net_state == NET_NO_SSID || net_state == NET_FAILED;
    if (held && radio_off) halo_stay_awake(false), held = false;
    bt_radio(!radio_off);  // Wi-Fi off: Bluetooth back (if it is on in Settings)
  }
}

static void net_poke() {  // any task: start connecting now (no waiting)
  net_last_ms = millis(), net_poked = true;
  if (net_task_h) xTaskNotifyGive(net_task_h);
}

// Not from the frame task: wait until Wi-Fi is up (true) or it failed / timed out (false). Pair with net_release()
static bool net_acquire(uint32_t timeout_ms) {
  __atomic_add_fetch(&net_users, 1, __ATOMIC_RELAXED);
  net_poke();
  const uint32_t t0 = millis();
  while (net_state != NET_UP) {
    if (net_state == NET_NO_SSID || millis() - t0 > timeout_ms || (net_state == NET_FAILED && millis() - t0 > 500)) {
      __atomic_sub_fetch(&net_users, 1, __ATOMIC_RELAXED);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

static void net_release() {
  net_last_ms = millis();
  __atomic_sub_fetch(&net_users, 1, __ATOMIC_RELAXED);
  if (net_task_h) xTaskNotifyGive(net_task_h);
}

static void net_release_soon() {  // a quick job (the weather): the radio need not linger for it
  net_last_ms = millis() - (uint32_t)cfg.net_idle * 1000 + 3000;
  __atomic_sub_fetch(&net_users, 1, __ATOMIC_RELAXED);
  if (net_task_h) xTaskNotifyGive(net_task_h);
}

// HTTPS GET into buf (NUL-terminated): the body's length, or −1. With the network acquired; not from the frame task
static int net_get(const char *url, char *buf, int cap) {
  xSemaphoreTake(net_tls_mx, portMAX_DELAY);
  esp_http_client_config_t c = {};
  c.url = url, c.timeout_ms = 10000, c.crt_bundle_attach = esp_crt_bundle_attach, c.buffer_size = 2048;
  esp_http_client_handle_t h = esp_http_client_init(&c);
  int n = -1;
  if (h && esp_http_client_open(h, 0) == ESP_OK) {
    esp_http_client_fetch_headers(h);
    const int st = esp_http_client_get_status_code(h);
    int got = 0, r;
    while (got < cap - 1 && (r = esp_http_client_read(h, buf + got, cap - 1 - got)) > 0) got += r;
    buf[got] = 0;
    n = st == 200 ? got : -1;
    if (st != 200) LOG("http: %d from %.60s\n", st, url);
  } else {
    LOG("http: could not open %.60s\n", url);
  }
  if (h) esp_http_client_cleanup(h);
  xSemaphoreGive(net_tls_mx);
  return n;
}

static void net_scan_cmd() {  // SCAN [{"ssid":…,"rssi":…,"open":…}, …] — for the controller's list
  if (net_state == NET_OFF) WiFi.mode(WIFI_STA);
  const int n = WiFi.scanNetworks();
  JBuf j;
  j.put("[");
  for (int i = 0; i < n; i++) {
    if (i) j.put(",");
    j.put("{"), j.key("ssid"), j.str(WiFi.SSID(i).c_str()), j.key("rssi"), j.f("%d", (int)WiFi.RSSI(i));
    j.key("open"), j.put(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false"), j.put("}");
  }
  j.put("]");
  WiFi.scanDelete();
  if (net_state == NET_OFF) WiFi.mode(WIFI_OFF);
  j.line("SCAN");
}

static void net_begin() {
  mbedtls_platform_set_calloc_free(net_tls_calloc, heap_caps_free);  // before any connection is opened
  // A binary semaphore, not a mutex: the assistant holds it for a whole session, and a FreeRTOS task gives back an
  // inherited priority only once it holds no mutex — it kept the Wi-Fi task's 23 for the session and starved the
  // speaker (halo_audio, 5): the reply crackled
  net_tls_mx = xSemaphoreCreateBinary();
  xSemaphoreGive(net_tls_mx);
  net_apply_tz();
  xTaskCreatePinnedToCore(net_task, "net", 6144, nullptr, 3, &net_task_h, 0);  // 6 KB: it starts Bluetooth too
  halo_command("time", [](const char *a) {  // time EPOCH [OFFSET_MIN] — from the web controller
    const long long t = atoll(a);
    if (t < 1700000000) return (void)Serial.println("ERR time bad epoch");
    const char *sp = strchr(a, ' ');
    if (sp && !cfg.tz[0] && net_auto_off == INT32_MIN) net_auto_off = atoi(sp + 1) * 60, net_apply_tz();
    net_time_set((time_t)t, "USB");
    Serial.println("OK time");
  });
  halo_command("wifi", [](const char *a) {  // wifi scan | on | off
    if (!strcmp(a, "scan")) net_scan_cmd();
    else if (!strcmp(a, "on")) net_poke(), Serial.println("OK wifi");
    else if (!strcmp(a, "off")) net_last_ms = millis() - 3600000u, net_poked = false, xTaskNotifyGive(net_task_h), Serial.println("OK wifi");
    else Serial.printf("wifi: %s %s\n", NET_NAME[net_state], net_state == NET_UP ? WiFi.localIP().toString().c_str() : "");
  });
}
