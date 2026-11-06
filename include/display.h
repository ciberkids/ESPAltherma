#ifndef ESPALTHERMA_DISPLAY_H
#define ESPALTHERMA_DISPLAY_H

// On-device screen for the M5 boards: a status bar, pages of heat pump values, a system page and
// a short log. Boards without a screen get empty stubs, so callers need no #ifdefs.
//
// Buttons: the front button (a tap on the Tough) shows the next page, the side button the previous
// one. A press on a dark screen only wakes it.

#include <stdarg.h>

#if defined(ARDUINO_M5Stick_C_Plus2) || defined(ARDUINO_M5Stick_C_Plus) || defined(ARDUINO_M5Stick_C) || defined(ARDUINO_M5Stack_Tough) || defined(ARDUINO_M5Stick_S3)
#define HAS_M5_SCREEN
#endif

#ifdef HAS_M5_SCREEN
#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "labeldef.h"
#include "restart.h"

extern PubSubClient client;

#ifdef ARDUINO_M5Stack_Tough
#define SCREEN_BRIGHTNESS 12
#define BUTTON_HINT "Tap the screen"
#else
#define SCREEN_BRIGHTNESS 100
#define BUTTON_HINT "Press M5"
#endif
#define SCREEN_ON_MS 30000UL      // the screen stays on this long after a button press
#define SCREEN_ON_BOOT_MS 40000UL // and this long after boot
#define DISPLAY_LOG_LINES 12
#define DISPLAY_LOG_COLS 48

namespace screen
{
const int HEADER_H = 18;
const int FOOTER_H = 12;
const int ROW_H = 21;       // one value per row, Font2 (16 px)
const int LOG_LINE_H = 10;  // Font0 (8 px)
const int PAD = 4;

const uint16_t HEADER_BG = 0x1108; // dark blue
const uint16_t LABEL_COLOR = TFT_LIGHTGREY;
const uint16_t DIM = TFT_DARKGREY;
const uint16_t LINE_COLOR = 0x2104; // row separator, almost black

// Every band (header, row, footer, log line) is drawn off-screen in this strip and pushed in one go,
// so the 1 Hz refresh does not flicker. It is only W x ROW_H pixels: a few kB on every board.
M5Canvas strip(&M5.Display);
bool stripReady = false;
int W = 0, H = 0;

char logLines[DISPLAY_LOG_LINES][DISPLAY_LOG_COLS];
uint16_t logRepeat[DISPLAY_LOG_LINES];
int logNext = 0, logCount = 0;

bool sleeping = false;
unsigned long offAt = SCREEN_ON_BOOT_MS;
int page = 0;
int pageBeforeLog = 0;
bool pageChosen = false; // set once the first values arrive or the user flips pages
bool dirty = true;
unsigned long lastDraw = 0;

bool regOk[256];         // result of the last read of each registry: stale values are greyed out
int cycles = 0, lastOk = 0, lastTotal = 0;
unsigned long lastGoodAt = 0;
float batteryV = 0;      // read once per cycle, not on every redraw
const int BATTERY_HISTORY = 5;
float batteryHistory[BATTERY_HISTORY]; // last readings, oldest first, to tell charging from draining
int batteryReadings = 0;

size_t valueCount() { return sizeof(labelDefs) / sizeof(labelDefs[0]); }
int rowsPerPage()
{
  int rows = (H - HEADER_H - FOOTER_H) / ROW_H;
  return rows < 1 ? 1 : rows;
}
int valuePages() { return (valueCount() + rowsPerPage() - 1) / rowsPerPage(); }
int systemPage() { return valuePages(); }
int logPage() { return valuePages() + 1; }
int pageCount() { return valuePages() + 2; }

struct Band
{
  lgfx::LGFXBase *g; // where to draw: the strip, or the display itself if the strip could not be allocated
  int top;           // y of the band's top edge on g
  int y;             // y of the band on the screen
  int h;
};

Band bandBegin(int y, int h, uint16_t bg)
{
  if (stripReady)
  {
    strip.fillRect(0, 0, W, h, bg);
    return {&strip, 0, y, h};
  }
  M5.Display.fillRect(0, y, W, h, bg);
  return {&M5.Display, y, y, h};
}

void bandEnd(const Band &b)
{
  if (!stripReady)
    return;
  M5.Display.setClipRect(0, b.y, W, b.h); // the strip is ROW_H tall; only push this band's part
  strip.pushSprite(0, b.y);
  M5.Display.clearClipRect();
}

void clearBand(int y, int h)
{
  if (h > 0)
    bandEnd(bandBegin(y, h, TFT_BLACK));
}

// Copies text into out, cut with ".." so it fits in maxW pixels with g's current font.
// Labels first lose a trailing sensor name in brackets, eg. "(R1T)".
void fitText(lgfx::LGFXBase *g, const char *text, int maxW, char *out, size_t outSize, bool dropBracket)
{
  strlcpy(out, text, outSize);
  if (g->textWidth(out) <= maxW)
    return;
  size_t n = strlen(out);
  char *bracket = strrchr(out, '(');
  if (dropBracket && bracket != nullptr && bracket > out && out[n - 1] == ')')
  {
    *bracket = 0;
    for (n = strlen(out); n > 0 && out[n - 1] == ' '; n--)
      out[n - 1] = 0;
    if (g->textWidth(out) <= maxW)
      return;
  }
  char base[64];
  strlcpy(base, out, sizeof(base));
  for (n = strlen(base); n > 0; n--)
  {
    snprintf(out, outSize, "%.*s..", (int)n, base);
    if (g->textWidth(out) <= maxW)
      return;
  }
  strlcpy(out, "..", outSize);
}

bool isNumber(const char *s)
{
  if (*s == '-')
    s++;
  return isdigit((unsigned char)*s);
}

// Unit shown after a numeric value, from the same data types Home Assistant discovery uses
const char *unitFor(const LabelDef &d)
{
  switch (d.dataType)
  {
  case 1: return "\xC2\xB0" "C"; // drawn as a small circle + "C": the built-in fonts have no degree sign
  case 2: return "bar";
  default: return nullptr;
  }
}

// Rough state of charge of a 1S LiPo from its voltage at light load (mV -> %). The curve is far from
// linear: most of the charge sits between 3.7 and 4.0 V. It reads high while the battery is charging.
int batteryPercent(float volts)
{
  static const uint16_t curve[][2] = {
      {3270, 0}, {3610, 5}, {3690, 10}, {3710, 15}, {3730, 20}, {3750, 25}, {3770, 30},
      {3790, 35}, {3800, 40}, {3820, 45}, {3840, 50}, {3850, 55}, {3870, 60}, {3910, 65},
      {3950, 70}, {3980, 75}, {4020, 80}, {4080, 85}, {4110, 90}, {4150, 95}, {4200, 100}};
  const int points = sizeof(curve) / sizeof(curve[0]);
  int mv = (int)(volts * 1000);
  if (mv <= curve[0][0])
    return 0;
  for (int i = 1; i < points; i++)
  {
    if (mv <= curve[i][0])
      return curve[i - 1][1] + (mv - curve[i - 1][0]) * (curve[i][1] - curve[i - 1][1]) / (curve[i][0] - curve[i - 1][0]);
  }
  return 100;
}

// "charging" / "draining" from the voltage trend over the last few cycles, "" if flat or unknown
const char *batteryTrend()
{
  if (batteryReadings < 3)
    return "";
  int n = batteryReadings < BATTERY_HISTORY ? batteryReadings : BATTERY_HISTORY;
  float change = batteryHistory[n - 1] - batteryHistory[0];
  return change > 0.02f ? " charging" : change < -0.02f ? " draining" : "";
}

void recordBattery(float volts)
{
  batteryV = volts;
  if (batteryReadings >= BATTERY_HISTORY)
    memmove(batteryHistory, batteryHistory + 1, sizeof(float) * (BATTERY_HISTORY - 1));
  batteryHistory[batteryReadings < BATTERY_HISTORY ? batteryReadings : BATTERY_HISTORY - 1] = volts;
  batteryReadings++;
}

// Seconds while under a minute, then minutes, hours and days: 45s, 12m, 3h 12m, 2d 3h
void formatUptime(char *out, size_t outSize)
{
  unsigned long s = (unsigned long)(esp_timer_get_time() / 1000000ULL); // 64-bit, no 49-day wrap like millis()
  if (s < 60)
    snprintf(out, outSize, "%lus", s);
  else if (s < 3600)
    snprintf(out, outSize, "%lum", s / 60);
  else if (s < 86400)
    snprintf(out, outSize, "%luh %lum", s / 3600, (s / 60) % 60);
  else
    snprintf(out, outSize, "%lud %luh", s / 86400, (s / 3600) % 24);
}

void formatAgo(unsigned long ms, char *out, size_t outSize)
{
  unsigned long s = ms / 1000;
  if (s < 120)
    snprintf(out, outSize, "%lus ago", s);
  else if (s < 7200)
    snprintf(out, outSize, "%lum ago", s / 60);
  else
    snprintf(out, outSize, "%luh ago", s / 3600);
}

// One "label ........ value unit" row. The label gives way to the value, down to 2/5 of the width.
void drawRow(int y, const char *label, const char *value, const char *unit, uint16_t valueColor)
{
  Band b = bandBegin(y, ROW_H, TFT_BLACK);
  lgfx::LGFXBase *g = b.g;
  g->setFont(&Font2);
  g->setTextDatum(TL_DATUM);
  int textY = b.top + (ROW_H - 16) / 2;

  bool degree = unit != nullptr && strncmp(unit, "\xC2\xB0", 2) == 0;
  const char *unitText = degree ? unit + 2 : unit;
  int unitW = unit != nullptr ? 3 + (degree ? 6 : 0) + g->textWidth(unitText) : 0;

  int labelW = g->textWidth(label);
  int labelMin = labelW < W * 2 / 5 ? labelW : W * 2 / 5;
  char v[32];
  fitText(g, value, W - 2 * PAD - 8 - labelMin - unitW, v, sizeof(v), false);
  int valueW = g->textWidth(v);
  int x = W - PAD - unitW - valueW;
  g->setTextColor(valueColor);
  g->drawString(v, x, textY);
  if (unit != nullptr)
  {
    int ux = x + valueW + 3;
    if (degree)
    {
      g->drawCircle(ux + 2, textY + 4, 2, DIM);
      ux += 6;
    }
    g->setTextColor(DIM);
    g->drawString(unitText, ux, textY);
  }

  char l[64];
  fitText(g, label, x - PAD - 8, l, sizeof(l), true);
  g->setTextColor(LABEL_COLOR);
  g->drawString(l, PAD, textY);
  g->drawFastHLine(PAD, b.top + ROW_H - 1, W - 2 * PAD, LINE_COLOR);
  bandEnd(b);
}

void drawHeader()
{
  Band b = bandBegin(0, HEADER_H, HEADER_BG);
  lgfx::LGFXBase *g = b.g;
  g->setFont(&Font2);
  g->setTextDatum(TL_DATUM);
  int textY = b.top + 1;

  bool wifi = WiFi.status() == WL_CONNECTED;
  int rssi = wifi ? WiFi.RSSI() : 0;
  char wifiText[16];
  if (wifi)
    snprintf(wifiText, sizeof(wifiText), "WiFi %d", rssi);
  else
    strlcpy(wifiText, "WiFi --", sizeof(wifiText));
  uint16_t wifiColor = !wifi ? TFT_RED : rssi > -67 ? TFT_GREEN : rssi > -80 ? TFT_YELLOW : TFT_ORANGE;

  const char *title = "ESPAltherma";
  bool compact = g->textWidth(title) + g->textWidth(wifiText) + g->textWidth("MQTT") + 3 * PAD + 20 > W;
  const char *wifiShown = compact ? wifiText + 5 : wifiText; // narrow screens: drop the words

  int x = W - PAD;
  if (!compact)
  {
    x -= g->textWidth("MQTT");
    g->setTextColor(TFT_WHITE);
    g->drawString("MQTT", x, textY);
    x -= 3;
  }
  g->fillCircle(x - 4, b.top + HEADER_H / 2, 4, client.connected() ? TFT_GREEN : TFT_RED);
  x -= 8 + 2 * PAD;
  x -= g->textWidth(wifiShown);
  g->setTextColor(wifiColor);
  g->drawString(wifiShown, x, textY);
  g->setTextColor(TFT_WHITE);
  g->drawString(title, PAD, textY);
  bandEnd(b);
}

void drawFooter()
{
  Band b = bandBegin(H - FOOTER_H, FOOTER_H, TFT_BLACK);
  lgfx::LGFXBase *g = b.g;
  g->setFont(&Font0);
  g->setTextDatum(TL_DATUM);
  int textY = b.top + 2;

  char status[48];
  char ago[16];
  uint16_t color = DIM;
  formatAgo(millis() - lastGoodAt, ago, sizeof(ago));
  if (cycles == 0)
    strlcpy(status, "waiting for heat pump", sizeof(status));
  else if (lastOk == 0)
  {
    strlcpy(status, "NO REPLY FROM HEAT PUMP", sizeof(status));
    color = TFT_RED;
  }
  else if (lastOk < lastTotal)
  {
    snprintf(status, sizeof(status), "%d/%d read, %s", lastOk, lastTotal, ago);
    color = TFT_YELLOW;
  }
  else
    snprintf(status, sizeof(status), "updated %s", ago);

  char p[12];
  snprintf(p, sizeof(p), "%d/%d", page + 1, pageCount());
  int pageW = g->textWidth(p);
  g->setTextColor(DIM);
  g->drawString(p, W - PAD - pageW, textY);

  char fitted[48];
  fitText(g, status, W - 2 * PAD - pageW - 6, fitted, sizeof(fitted), false);
  g->setTextColor(color);
  g->drawString(fitted, PAD, textY);
  bandEnd(b);
}

void drawValuesPage()
{
  int rows = rowsPerPage();
  for (int r = 0; r < rows; r++)
  {
    int y = HEADER_H + r * ROW_H;
    size_t i = page * rows + r;
    if (i >= valueCount())
    {
      clearBand(y, ROW_H);
      continue;
    }
    const LabelDef &d = labelDefs[i];
    bool fresh = cycles > 0 && regOk[(uint8_t)d.registryID];
    bool hasValue = d.asString[0] != 0;
    const char *value = hasValue ? d.asString : "--";
    uint16_t color = !fresh ? DIM : strcmp(value, "ON") == 0 ? TFT_GREEN : TFT_WHITE;
    drawRow(y, d.label, value, hasValue && isNumber(value) ? unitFor(d) : nullptr, color);
  }
  clearBand(HEADER_H + rows * ROW_H, H - FOOTER_H - HEADER_H - rows * ROW_H);
}

void drawSystemPage()
{
  int rows = rowsPerPage();
  int r = 0;
  auto row = [&](const char *label, const char *value) {
    if (r < rows)
      drawRow(HEADER_H + ROW_H * r++, label, value, nullptr, TFT_WHITE);
  };
  bool wifi = WiFi.status() == WL_CONNECTED;
  char buf[32];

  row("IP", wifi ? WiFi.localIP().toString().c_str() : "--");
  snprintf(buf, sizeof(buf), "%d dBm", WiFi.RSSI());
  row("WiFi", wifi ? buf : "not connected");
  row("Last reset", resetReasonName());
  formatUptime(buf, sizeof(buf));
  row("Uptime", buf);
  if (r < rows)
  {
    uint16_t color = TFT_WHITE;
    if (batteryV > 0.5)
    {
      int percent = batteryPercent(batteryV);
      snprintf(buf, sizeof(buf), "%d%% %.2fV%s", percent, batteryV, batteryTrend());
      color = percent >= 50 ? TFT_GREEN : percent >= 20 ? TFT_YELLOW : TFT_RED;
    }
    else
      strlcpy(buf, "--", sizeof(buf));
    drawRow(HEADER_H + ROW_H * r++, "Battery", buf, nullptr, color);
  }
  snprintf(buf, sizeof(buf), "%u kB", (unsigned)(ESP.getFreeHeap() / 1024));
  row("Free memory", buf);

  for (; r < rows; r++)
    clearBand(HEADER_H + ROW_H * r, ROW_H);
  clearBand(HEADER_H + rows * ROW_H, H - FOOTER_H - HEADER_H - rows * ROW_H);
}

void drawLogPage()
{
  int lines = (H - HEADER_H - FOOTER_H) / LOG_LINE_H;
  int shown = lines < logCount ? lines : logCount;
  for (int l = 0; l < lines; l++)
  {
    Band b = bandBegin(HEADER_H + l * LOG_LINE_H, LOG_LINE_H, TFT_BLACK);
    if (l < shown)
    {
      int idx = (logNext - shown + l + DISPLAY_LOG_LINES) % DISPLAY_LOG_LINES;
      char text[DISPLAY_LOG_COLS + 8];
      if (logRepeat[idx] > 1)
        snprintf(text, sizeof(text), "%s (x%u)", logLines[idx], logRepeat[idx]);
      else
        strlcpy(text, logLines[idx], sizeof(text));
      b.g->setFont(&Font0);
      b.g->setTextDatum(TL_DATUM);
      char fitted[DISPLAY_LOG_COLS + 8];
      fitText(b.g, text, W - 2 * PAD, fitted, sizeof(fitted), false);
      b.g->setTextColor(l == shown - 1 ? TFT_WHITE : LABEL_COLOR); // newest line in white
      b.g->drawString(fitted, PAD, b.top + 1);
    }
    bandEnd(b);
  }
  clearBand(HEADER_H + lines * LOG_LINE_H, H - FOOTER_H - HEADER_H - lines * LOG_LINE_H);
}

void draw()
{
  if (W == 0)
    return; // displayBegin() not called yet
  if (page >= pageCount())
    page = 0;
  drawHeader();
  if (page < valuePages())
    drawValuesPage();
  else if (page == systemPage())
    drawSystemPage();
  else
    drawLogPage();
  drawFooter();
  dirty = false;
  lastDraw = millis();
}
} // namespace screen

void displayLog(const char *fmt, ...)
{
  using namespace screen;
  char line[DISPLAY_LOG_COLS];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  int last = (logNext - 1 + DISPLAY_LOG_LINES) % DISPLAY_LOG_LINES;
  if (logCount > 0 && strcmp(logLines[last], line) == 0)
    logRepeat[last]++; // eg. repeated MQTT failures: one line with a counter
  else
  {
    strlcpy(logLines[logNext], line, DISPLAY_LOG_COLS);
    logRepeat[logNext] = 1;
    logNext = (logNext + 1) % DISPLAY_LOG_LINES;
    if (logCount < DISPLAY_LOG_LINES)
      logCount++;
  }
  dirty = true;
  if (!sleeping && page == logPage())
    draw(); // show progress right away, eg. while connecting at boot
}

void displayBegin()
{
  using namespace screen;
  M5.begin(); // keep first: it latches the power hold pin on the Plus2 and sets up the AXP192 on the StickC
  M5.Display.setRotation(1);
  M5.Display.setBrightness(SCREEN_BRIGHTNESS);
  M5.Display.setTextDatum(TL_DATUM);
  M5.Display.fillScreen(TFT_BLACK);
  W = M5.Display.width();
  H = M5.Display.height();
  strip.setColorDepth(16);
  stripReady = strip.createSprite(W, ROW_H) != nullptr;
  page = logPage(); // the log shows the boot progress until the first values arrive
  offAt = millis() + SCREEN_ON_BOOT_MS;
  displayLog("Last reset: %s", resetReasonName());
}

void displayWake()
{
  using namespace screen;
  if (sleeping)
  {
    M5.Display.wakeup();
    sleeping = false;
  }
  M5.Display.setBrightness(SCREEN_BRIGHTNESS);
  offAt = millis() + SCREEN_ON_MS;
  dirty = true;
}

// Switches to the log page (eg. while WiFi is down, so the hints are visible), or back.
void displayShowLog(bool show)
{
  using namespace screen;
  if (show && page != logPage())
  {
    pageBeforeLog = page;
    page = logPage();
  }
  else if (!show && page == logPage() && pageChosen)
    page = pageBeforeLog;
  dirty = true;
}

// Full-screen feedback while a new firmware arrives over WiFi, driven by the ArduinoOTA callbacks.
// The main loop is stopped during the upload, so these draw directly instead of waiting for displayTick().
namespace screen
{
int otaPercent = -1;
int otaPercentY = 0, otaBarY = 0, otaHintY = 0;
const int OTA_BAR_H = 12;
}

void displayOtaStart()
{
  using namespace screen;
  displayWake();
  otaPercent = -1;
  M5.Display.fillScreen(TFT_BLACK);
  drawHeader();
  bool roomy = H >= 120; // the 80 px StickC has no room for the title
  int y = HEADER_H + 4;
  M5.Display.setTextDatum(TC_DATUM);
  if (roomy)
  {
    M5.Display.setFont(&Font2);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("Firmware update", W / 2, y);
    y += 22;
  }
  otaPercentY = y;
  otaBarY = y + 30;
  otaHintY = roomy ? H - 18 : H - 10;
  M5.Display.drawRect(2 * PAD, otaBarY, W - 4 * PAD, OTA_BAR_H, LABEL_COLOR);
  M5.Display.setFont(roomy ? static_cast<const lgfx::IFont *>(&Font2) : static_cast<const lgfx::IFont *>(&Font0));
  M5.Display.setTextColor(TFT_YELLOW);
  M5.Display.drawString("Do not unplug", W / 2, otaHintY);
  M5.Display.setTextDatum(TL_DATUM);
}

void displayOtaProgress(unsigned int done, unsigned int total)
{
  using namespace screen;
  if (total == 0)
    return;
  int percent = (int)((uint64_t)done * 100 / total);
  if (percent == otaPercent)
    return; // redraw only when the number changes: about 100 times per upload
  otaPercent = percent;
  M5.Display.fillRect(2 * PAD + 2, otaBarY + 2, (W - 4 * PAD - 4) * percent / 100, OTA_BAR_H - 4, TFT_GREEN);
  char text[8];
  snprintf(text, sizeof(text), "%d%%", percent);
  M5.Display.fillRect(0, otaPercentY, W, 26, TFT_BLACK);
  M5.Display.setFont(&Font4);
  M5.Display.setTextDatum(TC_DATUM);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.drawString(text, W / 2, otaPercentY);
  M5.Display.setTextDatum(TL_DATUM);
}

void displayOtaEnd(bool ok, const char *message)
{
  using namespace screen;
  bool roomy = H >= 120;
  M5.Display.fillRect(0, otaHintY, W, H - otaHintY, TFT_BLACK);
  M5.Display.setFont(roomy ? static_cast<const lgfx::IFont *>(&Font2) : static_cast<const lgfx::IFont *>(&Font0));
  M5.Display.setTextDatum(TC_DATUM);
  M5.Display.setTextColor(ok ? TFT_GREEN : TFT_RED);
  M5.Display.drawString(message, W / 2, otaHintY);
  M5.Display.setTextDatum(TL_DATUM);
}

void displayRegisterRead(char registryID, bool ok)
{
  screen::regOk[(uint8_t)registryID] = ok;
}

void displayCycleDone(int ok, int total)
{
  using namespace screen;
  bool changed = cycles == 0 || ok != lastOk || total != lastTotal;
  cycles++;
  lastOk = ok;
  lastTotal = total;
  if (ok > 0)
    lastGoodAt = millis();
  recordBattery(M5.Power.getBatteryVoltage() / 1000.0f);
  if (changed)
  {
    if (ok == total)
      displayLog("Heat pump OK: %d/%d registers", ok, total);
    else if (ok == 0)
      displayLog("No reply from heat pump. Check X10A");
    else
      displayLog("Heat pump: only %d/%d registers", ok, total);
  }
  if (!pageChosen)
  {
    pageChosen = true;
    page = 0;
  }
  dirty = true;
}

// Polls the buttons, turns the screen off after a while and redraws it (at most once a second).
// With buttonPages false the front button does not flip pages: displayTick() returns true instead,
// and the caller decides what the press means (eg. stop retrying WiFi).
bool displayTick(bool buttonPages = true)
{
  using namespace screen;
  M5.update();
#ifdef ARDUINO_M5Stack_Tough
  bool next = M5.Touch.getCount() > 0 && M5.Touch.getDetail().wasPressed();
  bool prev = false;
#else
  bool next = M5.BtnA.wasPressed();
  bool prev = M5.BtnB.wasPressed();
#endif
  bool pressed = false;
  if (next || prev)
  {
    if (sleeping)
      displayWake(); // a press on a dark screen only wakes it
    else
    {
      offAt = millis() + SCREEN_ON_MS;
      if (!buttonPages)
        pressed = next;
      else
      {
        page = (page + (next ? 1 : pageCount() - 1)) % pageCount();
        pageChosen = true;
        dirty = true;
      }
    }
  }
  if (!sleeping && (long)(millis() - offAt) >= 0)
  {
    M5.Display.setBrightness(0);
    M5.Display.sleep();
    sleeping = true;
  }
  if (!sleeping && (dirty || millis() - lastDraw >= 1000))
    draw();
  return pressed;
}

#else // no screen

#define BUTTON_HINT ""
inline void displayBegin() {}
inline void displayLog(const char *, ...) {}
inline void displayWake() {}
inline void displayShowLog(bool) {}
inline void displayRegisterRead(char, bool) {}
inline void displayOtaStart() {}
inline void displayOtaProgress(unsigned int, unsigned int) {}
inline void displayOtaEnd(bool, const char *) {}
inline void displayCycleDone(int, int) {}
inline bool displayTick(bool = true) { return false; }

#endif // HAS_M5_SCREEN

#endif // ESPALTHERMA_DISPLAY_H
