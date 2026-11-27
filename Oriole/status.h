// status.h — the `status` command for the web controller (STATUS {json}: Wi-Fi, clock, assistant, weather, battery,
// memory) and what the app does when a setting changes (config.h calls cfg_changed). Included from Oriole.ino last.

#define HW_VERSION "1.2"

static void status_cmd(const char *) {
  JBuf j;
  j.put("{");
  j.key("version"), j.str(HW_VERSION);
  j.key("uptime"), j.f("%lu", (unsigned long)(millis() / 1000));
  // Wi-Fi
  j.key("wifi"), j.str(NET_NAME[net_state]);
  {
    CfgLock l;
    j.key("ssid"), j.str(net_state == NET_UP ? WiFi.SSID().c_str() : cfg.ssid);  // the network in use
  }
  if (net_state == NET_UP) {
    j.key("ip"), j.str(WiFi.localIP().toString().c_str());
    j.key("rssi"), j.f("%d", WiFi.RSSI());
  }
  // Clock
  j.key("clock_ok"), j.put(net_time_ok ? "true" : "false");
  j.key("epoch"), j.f("%lld", (long long)time(nullptr));
  const time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  char ts[40];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S UTC%z", &tm);  // the real offset (POSIX TZ strings flip the sign)
  j.key("local"), j.str(ts);
  {
    CfgLock l;
    j.key("tz"), j.str(cfg.tz);  // empty — auto
  }
  // Assistant
  j.key("ai"), j.str(ai_hold ? "listening" : AI_NAME[ai_state]);
  if (ai_state == AI_ERROR) j.key("ai_error"), j.str(ai_error);
  j.key("session"), j.put(ws_tls ? "true" : "false");
  // Weather
  j.key("weather_age"), j.f("%ld", wx_updated_ms ? (long)((millis() - wx_updated_ms) / 1000) : -1L);
  j.key("weather"), j.put("[");
  for (int i = 0; i < wx_n; i++) {
    if (i) j.put(",");
    j.put("{"), j.key("city"), j.str(wx_city_name[i]);
    if (wx_ok[i]) j.key("now"), j.f("%d", wx_days[i].now), j.key("hi"), j.f("%d", wx_days[i].hi), j.key("lo"), j.f("%d", wx_days[i].lo);
    j.put("}");
  }
  j.put("]");
  // Battery and memory
  const OrioleBattery b = oriole_battery();
  if (b.ok) {
    j.key("battery"), j.f("%d", b.percent), j.key("usb"), j.put(b.usb ? "true" : "false");
    j.key("charging"), j.put(b.charging ? "true" : "false");
  }
  j.key("heap"), j.f("%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  j.key("heap_min"), j.f("%u", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  j.key("psram"), j.f("%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  j.put("}");
  j.line("STATUS");
}

static void cfg_changed(const char *name) {  // from loop(), right after `set` saved it
  if (!strcmp(name, "volume")) oriole_audio_volume(cfg.vol);
  else if (!strcmp(name, "mic_gain")) oriole_mic_gain(cfg.gain);
  else if (!strcmp(name, "mic_channel")) oriole_mic_channel(cfg.mic_ch);
  else if (!strcmp(name, "tz")) net_apply_tz();
  else if (!strncmp(name, "wx_", 3)) wx_cmd_update = true, xTaskNotifyGive(wx_task_h);
  else if (!strcmp(name, "bt_on")) bt_apply();
  else if (!strcmp(name, "cal_url")) wx_cmd_update = true, xTaskNotifyGive(wx_task_h);  // read the new calendar
  else if (!strncmp(name, "ha_", 3)) home_refresh();
  else if (!strncmp(name, "wifi_", 5)) net_reconnect = true, net_poke();  // try the new network now
  else if (!strncmp(name, "ai_", 3) && strcmp(name, "ai_keep")) ai_cmd_close = true;  // the next session uses it
}

static void status_begin() {
  oriole_command("status", status_cmd);
}
