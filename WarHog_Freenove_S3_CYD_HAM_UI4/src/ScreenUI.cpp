#include "ScreenUI.h"

#include <math.h>

#include "SpiBusLock.h"   // Scheduler Phase 4: present() takes the cross-core SD lock (defensive)

#include "BatteryMonitor.h"
#include "BleScanner.h"
#include "AirTagMonitor.h"
#include "FlipperSniff.h"
#include "PwnagotchiDetect.h"
#include "RawCapture.h"
#include "DeauthDetector.h"
#include "WpsCheck.h"
#include "SessionStats.h"
#include "HamLogo.h"
#include "BoardPins.h"
#if defined(BOARD_W550)
#include "esp_heap_caps.h"   // side-panel recent-AP ring lives in PSRAM
#endif

#include "Globals.h"
#include "GPS.h"
#include "Scanner.h"
#include "MeshNode.h"
#include "ScreenTelemetry.h"
#include "SDUtils.h"
#include "WiFiManager.h"
#include "WigleUpload.h"

namespace {
constexpr int SCREEN_W = 240;   // width of the page column every page is laid out for
constexpr int SCREEN_H = 320;
#if defined(BOARD_W550)
// Landscape canvas: 800/1.5 wide, so present()'s x1.5 blit fills the whole 800x480
// panel. The page column stays at x 0..239 (all page code unchanged); the rest is
// the live side panel (drawSidePanel), laid out natively for landscape.
constexpr int CANVAS_W = 533;
constexpr int SIDE_X   = SCREEN_W;
constexpr int SIDE_W   = CANVAS_W - SIDE_X;   // 293
constexpr float PANEL_SCALE = 1.5f;
#else
constexpr int CANVAS_W = SCREEN_W;
#endif
constexpr int HEADER_H = 26;
constexpr int FOOTER_Y = 286;
constexpr int FOOTER_H = 34;
constexpr int CONTENT_H = FOOTER_Y - HEADER_H;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) |
                               ((g & 0xFC) << 3) |
                               (b >> 3));
}

// ---- Color themes (cosmetic only; chosen by cfg.theme, applied live, no reboot) -------
// These 11 palette tokens drive the whole UI. A handful of community-favorite dark
// palettes are provided; the legacy look is "midnight" (the default, byte-identical to
// the old constants). Switching a theme only recolors — no layout or behavior change.
// The C_* names below stay as accessors so none of the 340+ draw calls change.
struct UiTheme {
  const char* name;
  uint16_t bg, card, card2, input, border, text, muted, accent, warn, danger, blue;
};

static const UiTheme THEME_PRESETS[] = {
  // name           bg                card              card2             input             border            text                 muted                accent              warn                danger               blue
  { "midnight",  rgb565(10,14,19), rgb565(17,24,32), rgb565(21,29,40), rgb565(12,18,25), rgb565(30,42,58),  rgb565(226,234,244), rgb565(136,153,171), rgb565(45,212,191), rgb565(251,191,36), rgb565(251,113,133), rgb565(96,165,250)  },
  { "nord",      rgb565(46,52,64), rgb565(59,66,82), rgb565(67,76,94), rgb565(41,46,57), rgb565(76,86,106), rgb565(236,239,244), rgb565(97,110,136),  rgb565(136,192,208),rgb565(235,203,139),rgb565(191,97,106),  rgb565(129,161,193) },
  { "dracula",   rgb565(40,42,54), rgb565(52,55,70), rgb565(66,68,88), rgb565(33,34,44), rgb565(68,71,90),  rgb565(248,248,242), rgb565(98,114,164),  rgb565(189,147,249),rgb565(241,250,140),rgb565(255,85,85),   rgb565(139,233,253) },
  { "gruvbox",   rgb565(40,40,40), rgb565(60,56,54), rgb565(80,73,69), rgb565(29,32,33), rgb565(102,92,84), rgb565(235,219,178), rgb565(146,131,116), rgb565(250,189,47), rgb565(254,128,25), rgb565(251,73,52),   rgb565(131,165,152) },
  { "catppuccin",rgb565(30,30,46), rgb565(49,50,68), rgb565(69,71,90), rgb565(24,24,37), rgb565(88,91,112), rgb565(205,214,244), rgb565(108,112,134), rgb565(203,166,247),rgb565(249,226,175),rgb565(243,139,168), rgb565(137,180,250) },
  { "tokyonight",rgb565(26,27,38), rgb565(36,40,59), rgb565(47,51,77), rgb565(22,22,30), rgb565(59,66,97),  rgb565(192,202,245), rgb565(86,95,137),   rgb565(122,162,247),rgb565(224,175,104),rgb565(247,118,142), rgb565(125,207,255) },
};
constexpr int THEME_COUNT = sizeof(THEME_PRESETS) / sizeof(THEME_PRESETS[0]);

static UiTheme g_theme = THEME_PRESETS[0];   // active palette (midnight until config loads)

// Copy the preset named by cfg.theme into g_theme (falls back to midnight).
static void themeApplyFromConfig() {
  for (int i = 0; i < THEME_COUNT; ++i)
    if (cfg.theme == THEME_PRESETS[i].name) { g_theme = THEME_PRESETS[i]; return; }
  g_theme = THEME_PRESETS[0];
}

// Palette accessors — runtime now, read exactly like the old constants.
#define C_BG     (g_theme.bg)
#define C_CARD   (g_theme.card)
#define C_CARD_2 (g_theme.card2)
#define C_INPUT  (g_theme.input)
#define C_BORDER (g_theme.border)
#define C_TEXT   (g_theme.text)
#define C_MUTED  (g_theme.muted)
#define C_ACCENT (g_theme.accent)
#define C_WARN   (g_theme.warn)
#define C_DANGER (g_theme.danger)
#define C_BLUE   (g_theme.blue)

const char* const NAV_LABELS[] = { "HOME", "FILE", "SYS", "MRDR" };

constexpr int NAV_SLOTS = 4;
constexpr int NAV_SLOT_W = SCREEN_W / NAV_SLOTS;

// NODES page (Core mode) worker-list layout — shared by draw and hit-test so
// the tap targets always line up with what's drawn. All are unscrolled
// content-space Y (the coordinate handleTap passes to the page tap handlers).
constexpr int NODE_METRIC_TOP   = 60;
constexpr int NODE_PROFILES_TOP = 112;   // "PROFILES" bar
constexpr int NODE_PROFILES_H   = 26;
constexpr int NODE_LIST_TOP     = 146;   // first worker card
constexpr int NODE_CARD_H       = 62;
constexpr int NODE_CARD_PITCH   = 69;

inline uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  const int rr = ar + (int)lroundf((br - ar) * t);
  const int gg = ag + (int)lroundf((bg - ag) * t);
  const int bl = ab + (int)lroundf((bb - ab) * t);
  return (uint16_t)((rr << 11) | (gg << 5) | bl);
}

inline uint16_t speedColor(float t) {
  const uint16_t c0 = rgb565(45, 212, 191), c1 = rgb565(96, 165, 250), c2 = rgb565(167, 139, 250);
  return (t < 0.5f) ? lerp565(c0, c1, t * 2.0f) : lerp565(c1, c2, (t - 0.5f) * 2.0f);
}

inline float pulse01(uint32_t periodMs) {
  const float x = (float)(millis() % periodMs) / (float)periodMs;
  return x < 0.5f ? x * 2.0f : (1.0f - x) * 2.0f;
}

inline void fillVGrad(GfxSprite& s, int x, int y, int w, int h, uint16_t ctop, uint16_t cbot) {
  const int d = (h > 1) ? h - 1 : 1;
  for (int i = 0; i < h; ++i) s.drawFastHLine(x, y + i, w, lerp565(ctop, cbot, (float)i / d));
}

String shortText(const String& value, size_t maxChars) {
  if (value.length() <= maxChars) return value;
  return value.substring(0, maxChars - 1) + "~";
}

const char* wifiModeLabel() {
  switch (WiFi.getMode()) {
    case WIFI_OFF: return "OFF";
    case WIFI_STA: return "STA";
    case WIFI_AP: return "AP";
    case WIFI_AP_STA: return "AP+STA";
    default: return "?";
  }
}

const char* heading8(double degrees) {
  if (!isfinite(degrees)) return "--";
  static const char* labels[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int index = static_cast<int>((degrees + 22.5) / 45.0) & 7;
  return labels[index];
}

struct Choice {
  const char* value;
  const char* label;
};

enum class SettingKind : uint8_t { Header, Choice, Text, Secret, Number };

struct SettingDef {
  SettingKind kind;
  const char* label;
  const char* key;
  const Choice* choices;
  uint8_t choiceCount;
  bool reboot;
  const char* hint;
};

const Choice SCAN_MODES[] = {{"aggressive", "Aggressive (4.5s)"}, {"powersaving", "Power saving (12s)"}};
const Choice SPEED_UNITS[] = {{"kmh", "km/h"}, {"mph", "mph"}};
const Choice BOARDS[] = {{"auto", "Auto detect"}, {"s3", "XIAO S3"}, {"exp", "S3 Expansion"},
                         {"c5", "XIAO C5"}, {"c6", "XIAO C6"}, {"c3", "XIAO C3"}};
const Choice ON_OFF[] = {{"false", "Disabled"}, {"true", "Enabled"}};
const Choice BLE_APPS[] = {{"biscuit", "Biscuit Manager"}, {"wardrivego", "Wardrive Go"}};
const Choice MESH_MODES[] = {{"none", "None (solo)"}, {"node", "Node (scanner)"}, {"core", "Core (coordinator)"}, {"off", "Off (radio free)"}};
const Choice ROTATION[] = {{"false", "Normal"}, {"true", "Rotated 180"}};
const Choice THEMES[] = {{"midnight", "Midnight"}, {"nord", "Nord"}, {"dracula", "Dracula"},
                         {"gruvbox", "Gruvbox"}, {"catppuccin", "Catppuccin"}, {"tokyonight", "Tokyo Night"}};

#define CHOICES(a) a, static_cast<uint8_t>(sizeof(a) / sizeof(a[0]))

const SettingDef SETTINGS[] = {
  {SettingKind::Header, "UPLOADS", nullptr, nullptr, 0, false, nullptr},
  {SettingKind::Secret, "WIGLE TOKEN", "wigleBasicToken", nullptr, 0, false, "'Encoded for use' token"},
  {SettingKind::Secret, "WDGOWARS API KEY", "wdgwarsApiKey", nullptr, 0, false, "Empty = disabled"},
  {SettingKind::Number, "MAX BOOT UPLOADS", "maxBootUploads", nullptr, 0, false, "-1 all, 0 off, 1+ limit"},
  {SettingKind::Choice, "AUTO-START AFTER UPLOAD", "autoStartAfterUpload", CHOICES(ON_OFF), true, nullptr},

  {SettingKind::Header, "WI-FI", nullptr, nullptr, 0, false, nullptr},
  {SettingKind::Text, "HOME SSID", "homeSsid", nullptr, 0, true, nullptr},
  {SettingKind::Secret, "HOME PSK", "homePsk", nullptr, 0, true, nullptr},
  {SettingKind::Text, "WARDRIVER SSID", "wardriverSsid", nullptr, 0, true, nullptr},
  {SettingKind::Secret, "WARDRIVER PSK", "wardriverPsk", nullptr, 0, true, "8+ characters"},

  {SettingKind::Header, "SCANNING", nullptr, nullptr, 0, false, nullptr},
  {SettingKind::Choice, "SCAN MODE", "scanMode", CHOICES(SCAN_MODES), false, nullptr},
  {SettingKind::Choice, "BLUETOOTH SCAN", "bluetoothScan", CHOICES(ON_OFF), false, "Log BLE for WDG points"},
  {SettingKind::Choice, "BISCUIT REPORT", "biscuitReport", CHOICES(ON_OFF), false, "ESP-NOW to a Biscuit Core"},
  {SettingKind::Choice, "CORE PHONE APP", "blePersonality", CHOICES(BLE_APPS), true, "Phone app: Biscuit / Wardrive Go"},
  {SettingKind::Choice, "PHONE APP SOLO", "blePhoneSolo", CHOICES(ON_OFF), false, "Stream solo scans over BLE"},
  {SettingKind::Choice, "SPEED UNITS", "speedUnits", CHOICES(SPEED_UNITS), false, nullptr},
  {SettingKind::Choice, "MESH MODE ON BOOT", "meshModeOnBoot", CHOICES(MESH_MODES), true, nullptr},

  {SettingKind::Header, "DEVICE", nullptr, nullptr, 0, false, nullptr},
  {SettingKind::Text, "DEVICE NAME", "deviceName", nullptr, 0, false, "Letters, digits, _ and -"},
  {SettingKind::Text, "BEACON SSID", "beaconSsid", nullptr, 0, false, "Custom beacon-spam SSID"},
  {SettingKind::Number, "GPS BAUD", "gpsBaud", nullptr, 0, true, "e.g. 9600, 38400, 115200"},
  {SettingKind::Choice, "BOARD", "board", CHOICES(BOARDS), true, nullptr},
  {SettingKind::Number, "BATTERY ADC PIN", "battPin", nullptr, 0, false, "FNK0104B = 9, -1 = off"},
  {SettingKind::Choice, "BATTERY TEST", "batteryTest", CHOICES(ON_OFF), true, nullptr},
  {SettingKind::Choice, "ROTATE SCREEN", "rotateScreen180", CHOICES(ROTATION), true, nullptr},
  {SettingKind::Choice, "COLOR THEME", "theme", CHOICES(THEMES), false, "UI colors only"},
};

#undef CHOICES

constexpr int SETTING_COUNT = sizeof(SETTINGS) / sizeof(SETTINGS[0]);
constexpr int SYS_TOP = 6;
constexpr int SYS_HEADER_H = 17;
constexpr int SYS_ROW_H = 32;
constexpr int SYS_CARD_H = 28;
constexpr int SYS_INFO_H = 58;
constexpr int SYS_BUTTON_H = 33;
constexpr int SYS_TOOLS_H = 796;

int settingHeight(const SettingDef& def) {
  return def.kind == SettingKind::Header ? SYS_HEADER_H : SYS_ROW_H;
}

int systemListBottom() {
  int y = SYS_TOP + SYS_TOOLS_H;
  for (const SettingDef& def : SETTINGS) y += settingHeight(def);
  return y;
}

int systemInfoY() { return systemListBottom() + 4; }
int systemButtonsY() { return systemInfoY() + SYS_INFO_H + 6; }

String boolText(bool v) { return v ? "true" : "false"; }

int settingIndexByKey(const char* key) {
  for (int i = 0; i < SETTING_COUNT; ++i) {
    if (SETTINGS[i].key && strcmp(SETTINGS[i].key, key) == 0) return i;
  }
  return -1;
}

String settingValue(const char* key) {
  const String k(key);
  if (k == "wigleBasicToken") return cfg.wigleBasicToken;
  if (k == "wdgwarsApiKey") return cfg.wdgwarsApiKey;
  if (k == "maxBootUploads") return String(cfg.maxBootUploads);
  if (k == "autoStartAfterUpload") return boolText(cfg.autoStartAfterUpload);
  if (k == "homeSsid") return cfg.homeSsid;
  if (k == "homePsk") return cfg.homePsk;
  if (k == "wardriverSsid") return cfg.wardriverSsid;
  if (k == "wardriverPsk") return cfg.wardriverPsk;
  if (k == "scanMode") return cfg.scanMode;
  if (k == "bluetoothScan") return boolText(cfg.bluetoothScan);
  if (k == "biscuitReport") return boolText(cfg.biscuitReport);
  if (k == "blePersonality") return cfg.blePersonality;
  if (k == "blePhoneSolo") return boolText(cfg.blePhoneSolo);
  if (k == "speedUnits") return cfg.speedUnits;
  if (k == "theme") return cfg.theme;
  if (k == "meshModeOnBoot") return cfg.meshModeOnBoot;
  if (k == "deviceName") return cfg.deviceName;
  if (k == "beaconSsid") return cfg.beaconSsid;
  if (k == "gpsBaud") return String(cfg.gpsBaud);
  if (k == "board") return cfg.board;
  if (k == "battPin") return String(cfg.battPin);
  if (k == "batteryTest") return boolText(cfg.batteryTest);
  if (k == "rotateScreen180") return boolText(cfg.rotateScreen180);
  return String();
}

String settingDisplay(const SettingDef& def) {
  const String value = settingValue(def.key);
  switch (def.kind) {
    case SettingKind::Choice:
      for (uint8_t i = 0; i < def.choiceCount; ++i) {
        if (value == def.choices[i].value) return def.choices[i].label;
      }
      return value;
    case SettingKind::Secret:
      if (!value.length()) return "(not set)";
      return "******** (" + String(value.length()) + " chars)";
    case SettingKind::Text:
      return value.length() ? shortText(value, 33) : String("(not set)");
    default:
      return value;
  }
}

constexpr int ED_BOX_Y = 32;
constexpr int ED_BOX_H = 62;
constexpr int ED_HINT_Y = 101;
constexpr int ED_KEYS_Y = 116;
constexpr int ED_KEY_H = 40;
constexpr int ED_BOTTOM_Y = ED_KEYS_Y + 4 * ED_KEY_H;
constexpr int ED_CHARS_PER_LINE = 35;
constexpr size_t ED_MAX_LEN = 128;

const char* const TEXT_KEYS[3][4] = {
  {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.@"},
  {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_.@"},
  {"!\"#$%&'()*", "+,-./:;<=>", "?@[\\]^_`{|", "}~"},
};
const char* const NUMBER_KEYS[4] = {"123", "456", "789", "-0"};
const char* const LAYER_NEXT_LABEL[3] = {"ABC", "#+=", "abc"};
const char* const BOTTOM_KEYS[6] = {nullptr, "SPACE", "DEL", "CLR", "ESC", "OK"};
}

ScreenUI screenUI;

void ScreenUI::begin() {
  themeApplyFromConfig();   // apply the saved color theme before the first paint
#if !defined(BOARD_W550)
  pinMode(BoardPins::BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BoardPins::BACKLIGHT_PIN, HIGH);
#endif

  tft.init();
  tft.setRotation(0);
  tft.setTextWrap(false);

  frame.setColorDepth(16);
#if defined(BOARD_W550)
  frame.setPsram(true);
  tft.setBrightness(255);
  tft.fillScreen(C_BG);
#else
  frame.setAttribute(PSRAM_ENABLE, true);
#endif
  spriteReady = frame.createSprite(CANVAS_W, SCREEN_H) != nullptr;

#if !defined(BOARD_W550)
  touch.begin();  // W550 touch (GT911) is initialized by LovyanGFX in tft.init()
#endif
  showSplash();
  redraw = true;
}

void ScreenUI::present() {
  spiBusLock();   // defensive: no current board shares the TFT and SD bus (see SpiBusLock.h)
#if defined(BOARD_W550)
  // The UI renders into a 533x320 landscape canvas (page column + side panel).
  // Scale it x1.5 to fill the 800x480 panel edge to edge.
  frame.setPivot(CANVAS_W * 0.5f, SCREEN_H * 0.5f);
  frame.pushRotateZoom(&tft, tft.width() * 0.5f, tft.height() * 0.5f, 0.0f,
                       PANEL_SCALE, PANEL_SCALE);
#else
  frame.pushSprite(0, 0);
#endif
  spiBusUnlock();
}

void ScreenUI::showSplash() {
  if (!spriteReady) {
    tft.fillScreen(C_BG);
    return;
  }

#if defined(BOARD_W550)
  // Center the 240-wide splash on the landscape canvas (viewport = origin shift + clip).
  frame.fillSprite(C_BG);
  frame.setViewport((CANVAS_W - SCREEN_W) / 2, 0, SCREEN_W, SCREEN_H);
#endif
  const int cx = 120, cy = 116;
  const int Rmax = 116, Rrim = 158;
  const uint32_t DUR = 5000;

  const int NBLIP = 16;
  float blipAng[NBLIP];
  int   blipRad[NBLIP];
  for (int i = 0; i < NBLIP; ++i) {
    blipAng[i] = (random(3600) / 10.0f) * DEG_TO_RAD;
    blipRad[i] = 120 + random(34);
  }

  const uint32_t start = millis();
  while (true) {
    const uint32_t el = millis() - start;
    if (el >= DUR) break;
    frame.fillSprite(C_BG);

    for (int r = 24; r <= Rrim; r += 24) frame.drawCircle(cx, cy, r, C_CARD);
    frame.drawFastHLine(cx - Rrim, cy, Rrim * 2, C_CARD);
    frame.drawFastVLine(cx, cy - Rrim, Rrim * 2, C_CARD);

    for (int p = 0; p * 650 < (int)el; ++p) {
      const float pr = (el - p * 650) * 0.15f;
      if (pr > 8 && pr < 168) frame.drawCircle(cx, cy, (int)pr, lerp565(C_ACCENT, C_BG, pr / 168.0f));
    }

    const float sweep = millis() / 450.0f;
    for (int d = 0; d <= 52; ++d) {
      const float a = sweep - d * DEG_TO_RAD;
      frame.drawLine(cx, cy, cx + (int)(cosf(a) * Rrim), cy + (int)(sinf(a) * Rrim),
                     lerp565(C_ACCENT, C_BG, (float)d / 52.0f));
    }
    frame.drawLine(cx, cy, cx + (int)(cosf(sweep) * Rrim), cy + (int)(sinf(sweep) * Rrim), C_TEXT);
    frame.fillCircle(cx + (int)(cosf(sweep) * Rrim), cy + (int)(sinf(sweep) * Rrim), 2, C_TEXT);

    for (int i = 0; i < NBLIP; ++i) {
      float da = fmodf(sweep - blipAng[i], (float)TWO_PI);
      if (da < 0) da += (float)TWO_PI;
      const float bright = da < 1.5f ? (1.0f - da / 1.5f) : 0.0f;
      const int bx = cx + (int)(cosf(blipAng[i]) * blipRad[i]);
      const int by = cy + (int)(sinf(blipAng[i]) * blipRad[i]);
      const uint16_t base = speedColor((float)(i % 5) / 4.0f);
      if (bright > 0.06f) {
        frame.fillCircle(bx, by, 2, lerp565(C_CARD, base, bright));
        if (bright > 0.82f) frame.drawCircle(bx, by, 4, base);
      } else {
        frame.drawPixel(bx, by, C_CARD_2);
      }
    }

    frame.fillCircle(cx, cy, Rmax, C_BG);
    frame.drawCircle(cx, cy, Rmax, C_BORDER);
    const float lf = (el > 250) ? min(1.0f, (el - 250) / 900.0f) : 0.0f;
    const uint16_t logoCol = lerp565(C_BG, rgb565(242, 236, 222), lf);
    frame.drawBitmap(cx - HAM_LOGO_W / 2, cy - HAM_LOGO_H / 2, HAM_LOGO, HAM_LOGO_W, HAM_LOGO_H, logoCol);

    float f = (el > 350) ? min(1.0f, (el - 350) / 500.0f) : 0.0f;
    frame.setTextDatum(MC_DATUM);
    const char* TITLE = "WARHOG CORE";
    const int ty = 258;
    frame.setTextColor(lerp565(C_BG, C_ACCENT, 0.28f * f));
    const int8_t gx[] = {-2, 2, 0, 0, -1, 1, -1, 1};
    const int8_t gy[] = {0, 0, -2, 2, -1, 1, 1, -1};
    for (int i = 0; i < 8; ++i) frame.drawString(TITLE, cx + gx[i], ty + gy[i], 4);
    const int split = 2 + (int)(pulse01(650) * 2.0f);
    frame.setTextColor(lerp565(C_BG, C_BLUE, f));
    frame.drawString(TITLE, cx - split, ty, 4);
    frame.setTextColor(lerp565(C_BG, rgb565(236, 72, 153), f));
    frame.drawString(TITLE, cx + split, ty, 4);
    frame.setTextColor(lerp565(C_BG, C_TEXT, f));
    frame.drawString(TITLE, cx, ty, 4);
    frame.setTextColor(lerp565(C_BG, C_ACCENT, f));
    frame.drawString("[", cx - 94, ty, 4);
    frame.drawString("]", cx + 94, ty, 4);

    frame.setTextColor(lerp565(C_BG, C_MUTED, f), C_BG);
    frame.drawString(FIRMWARE_VERSION, cx, 286, 1);

    const int barW = (int)(180.0f * min(1.0f, (float)el / DUR));
    frame.drawRoundRect(29, 300, 182, 8, 3, C_BORDER);
    if (barW > 2) fillVGrad(frame, 30, 301, barW, 6, lerp565(C_ACCENT, C_BLUE, 0.4f), C_ACCENT);

    present();
    delay(24);
  }
#if defined(BOARD_W550)
  frame.resetViewport();
#endif
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::requestRedraw() { redraw = true; }

void ScreenUI::update(float speedValue) {
  displayedSpeed = speedValue;
  if (!spriteReady) return;

  uint32_t refreshMs = 1000;
  switch (page) {
    case Page::Home: refreshMs = 300; break;
    case Page::Nodes: refreshMs = 500; break;
    case Page::Files: refreshMs = 3000; break;
    case Page::System: refreshMs = 700; break;
    case Page::Marauder: refreshMs = 400; break;
    case Page::Tools: refreshMs = 400; break;
  }
  if (speedoMode) refreshMs = 250;
#if defined(BOARD_W550)
  refreshMs = min<uint32_t>(refreshMs, 1000);   // keep the live side panel current on slow pages
#endif

  if (!redraw && millis() - lastDrawMs < refreshMs) return;
  draw();
  redraw = false;
  lastDrawMs = millis();
}

void ScreenUI::handleTouch() {
#if defined(BOARD_W550)
  // GT911 is driven by LovyanGFX in physical 800x480 panel space. The UI logic
  // below works in canvas space, so invert the present() x1.5 blit. Only the
  // page column (x < SCREEN_W) is interactive: a touch that starts on the side
  // panel is ignored, and a drag that strays onto it is clamped to the column
  // edge so it isn't mistaken for a release.
  TouchPoint point;
  {
    int32_t px = 0, py = 0;
    if (tft.getTouch(&px, &py)) {
      const int sx = int(px / PANEL_SCALE);
      const int sy = int(py / PANEL_SCALE);
      if (sx >= 0 && sy >= 0 && sy < SCREEN_H && (sx < SCREEN_W || touchActive)) {
        point.pressed = true;
        point.x = min(sx, SCREEN_W - 1);
        point.y = sy;
      }
    }
  }
#else
  const TouchPoint point = touch.read();
#endif

  if (point.pressed) {
    if (!touchActive) {
      touchActive = true;
      touchStartX = touchLastX = point.x;
      touchStartY = touchLastY = point.y;
      touchStartMs = millis();
      return;
    }
    const int16_t dy = point.y - touchLastY;
    if (!editing && !speedoMode && !(page == Page::Files && csvPreview) &&
        point.y >= HEADER_H && point.y < FOOTER_Y &&
        abs(dy) >= 2) {
      setScroll(activeScroll() - dy);
      redraw = true;
    }
    touchLastX = point.x;
    touchLastY = point.y;
    return;
  }

  if (!touchActive) return;

  const int16_t dx = touchLastX - touchStartX;
  const int16_t dy = touchLastY - touchStartY;
  const uint32_t held = millis() - touchStartMs;
  touchActive = false;

  if (editing) {
    if (abs(dx) + abs(dy) < 28 && held < 1200) handleEditorTap(touchStartX, touchStartY);
    return;
  }
  if (speedoMode) {
    if (abs(dx) + abs(dy) < 28 && held < 1200) { speedoMode = false; redraw = true; }
    return;
  }

  if (page == Page::Files && csvPreview) {
    if (abs(dx) > 35 && abs(dx) > abs(dy)) {
      int next = static_cast<int>(csvColumns) + (dx < 0 ? 1 : -1);
      while (next < 0) next += 3;
      while (next >= 3) next -= 3;
      csvColumns = static_cast<CsvColumns>(next);
      redraw = true;
      return;
    }
    if (abs(dy) > 30 && abs(dy) > abs(dx)) {
      if (dy < 0) csvRowOffset += 4;
      else        csvRowOffset = csvRowOffset > 4 ? csvRowOffset - 4 : 0;
      redraw = true;
      return;
    }
  }

  if (abs(dx) + abs(dy) < 28 && held < 1200) {
    handleTap(touchStartX, touchStartY);
  }
}

void ScreenUI::handleTap(int16_t x, int16_t y) {
  if (y >= FOOTER_Y) {
    static const Page NAV_PAGES[NAV_SLOTS] = {
      Page::Home, Page::Files, Page::System, Page::Marauder
    };
    const Page target = NAV_PAGES[min(NAV_SLOTS - 1, max(0, x / NAV_SLOT_W))];

    if (target == Page::Files && page == Page::Files && csvPreview) {
      csvPreview = false;
      csvRowOffset = 0;
      csvColumns = CsvColumns::Network;
      scroll[static_cast<int>(Page::Files)] = 0;
      redraw = true;
      return;
    }
    setPage(target);
    return;
  }

  const int contentY = y - HEADER_H + activeScroll();

  switch (page) {
    case Page::Home: handleHomeTap(x, contentY); break;
    case Page::Nodes: handleNodesTap(x, contentY); break;
    case Page::Files: handleFilesTap(x, contentY); break;
    case Page::System: handleSystemTap(x, contentY); break;
    case Page::Marauder:
      switch (attackScreen) {
        case AttackScreen::Beacon:    handleBeaconPageTap(x, contentY); break;
        case AttackScreen::Deauth:    handleDeauthPageTap(x, contentY); break;
        case AttackScreen::Sniff:     handleChannelPageTap(x, contentY, AttackMode::ProbeSniff); break;
        case AttackScreen::Capture:   handleCapturePageTap(x, contentY); break;
        case AttackScreen::Analyzer:  handleWifiAnalyzerTap(contentY); break;
        case AttackScreen::Wps:       handleWpsPageTap(x, contentY); break;
        case AttackScreen::Ble:       handleBlePageTap(x, contentY); break;
        case AttackScreen::DeauthDet: handleDeauthDetPageTap(x, contentY); break;
        case AttackScreen::Pmkid:     handlePmkidPageTap(x, contentY); break;
        default:                      handleMarauderTap(x, contentY); break;
      }
      break;
    default: break;
  }
}

// ── MARAUDER tool menu rows ──────────────────────────────────────────────────
// Data-driven so WiFi attacks, the analyzer/WPS views, BLE spam and the passive
// BLE detectors can share one scrollable menu.
namespace {
enum class MRow : uint8_t {
  Beacon, Deauth, Sniff, Capture, Analyzer, Wps, Ble,
  AirTag, Flipper, Pwn, DeauthDet, Pmkid
};
constexpr int MARAUDER_ROWS = 12;
const char* const MARAUDER_LABELS[MARAUDER_ROWS] = {
  "BEACON SPAM", "DEAUTH", "PROBE SNIFF", "CAPTURE",
  "WIFI ANALYZER", "WPS CHECK", "BLE SPAM",
  "AIRTAG MONITOR", "FLIPPER SNIFF", "PWNAGOTCHI",
  "DEAUTH DETECT", "PMKID CAPTURE"
};

bool marauderRowRunning(int i) {
  const AttackMode cur = marauderCurrentAttack();
  switch (static_cast<MRow>(i)) {
    case MRow::Beacon:    return cur == AttackMode::BeaconSpam;
    case MRow::Deauth:    return cur == AttackMode::Deauth;
    case MRow::Sniff:     return cur == AttackMode::ProbeSniff;
    case MRow::Capture:   return cur == AttackMode::PcapCapture || cur == AttackMode::RawCapture;
    case MRow::Ble:       return bleSpamActive();
    case MRow::AirTag:    return airTagMonitorActive();
    case MRow::Flipper:   return flipperSniffActive();
    case MRow::Pwn:       return pwnagotchiDetectActive();
    case MRow::DeauthDet: return cur == AttackMode::DeauthDetect;
    case MRow::Pmkid:     return cur == AttackMode::Pmkid;
    default:              return false;
  }
}
}  // namespace

void ScreenUI::handleMarauderTap(int16_t x, int16_t contentY) {
  (void)x;
  const int ROW_H = 32;
  const int CARD_Y = 6;

  if (contentY < CARD_Y + 20) return;
  const int row = (contentY - (CARD_Y + 20)) / ROW_H;
  if (row < 0 || row >= MARAUDER_ROWS) return;

  const bool busy = marauderAttackActive() || bleSpamActive() ||
                    airTagMonitorActive() || flipperSniffActive() ||
                    pwnagotchiDetectActive();
  if (busy && !marauderRowRunning(row)) return;

  if (row <= static_cast<int>(MRow::Pmkid)) {
    scroll[static_cast<int>(Page::Marauder)] = 0;
  }

  switch (static_cast<MRow>(row)) {
    case MRow::Beacon:
      beaconSrc = marauderBeaconSource();
      beaconHop = marauderBeaconHop();
      attackScreen = AttackScreen::Beacon;
      break;
    case MRow::Deauth:
      if (!marauderAttackActive()) { marauderRefreshScanResults(); bssidPickerOffset = 0; }
      attackScreen = AttackScreen::Deauth;
      break;
    case MRow::Sniff:    attackScreen = AttackScreen::Sniff;   break;
    case MRow::Capture:  attackScreen = AttackScreen::Capture; break;
    case MRow::Analyzer: attackScreen = AttackScreen::Analyzer; break;
    case MRow::Wps:      wpsOffset = 0; attackScreen = AttackScreen::Wps; break;
    case MRow::Ble:      attackScreen = AttackScreen::Ble; break;
    case MRow::AirTag:
      if (airTagMonitorActive()) airTagMonitorStop(); else airTagMonitorStart();
      break;
    case MRow::Flipper:
      if (flipperSniffActive()) flipperSniffStop(); else flipperSniffStart();
      break;
    case MRow::Pwn:
      if (pwnagotchiDetectActive()) pwnagotchiDetectStop(); else pwnagotchiDetectStart();
      break;
    case MRow::DeauthDet:
      attackScreen = AttackScreen::DeauthDet;
      break;
    case MRow::Pmkid:
      if (!marauderAttackActive()) { marauderRefreshScanResults(); pmkidListOffset = 0; }
      attackScreen = AttackScreen::Pmkid;
      break;
  }
  redraw = true;
}

void ScreenUI::handleHomeTap(int16_t x, int16_t contentY) {
  if (contentY >= 24 && contentY <= 43) {
    const int i = (x - 9) / 44;
    switch (i) {
      case 0: setWardMode();        break;
      case 1: enterCoreUiMode();    break;
      case 2: enterNodeUiMode();    break;
      case 3: setWebMode();         break;
      case 4: enterRadioIdleMode(); break;
      default: return;
    }
    return;
  }

  // In Core mode the STATUS card drills into the cluster view (node list +
  // per-node sub-cluster editor + saved profiles).
  if (meshCoreActive && contentY >= 56 && contentY <= 106) {
    setPage(Page::Nodes);
    return;
  }
  if (contentY >= 126 && contentY <= 145 && x >= 13) {
    const int i = (x - 13) / 73;
    if (i == 0)      cfg.scan24 = !cfg.scan24;
    else if (i == 1) cfg.scan5 = !cfg.scan5;
    else if (i == 2) cfg.bluetoothScan = !cfg.bluetoothScan;
    else return;
    persistConfig();
  }
}

void ScreenUI::handleNodesTap(int16_t x, int16_t contentY) {
  if (meshCoreActive && profilesView) { handleProfilesTap(x, contentY); return; }
  if (meshCoreActive && nodeDetail)   { handleNodeDetailTap(x, contentY); return; }

  // Role selector (top card).
  if (contentY >= 8 && contentY <= 45) {
    if (x < 68) enterSoloMode();
    else if (x < 122) enterCoreUiMode();
    else if (x < 176) enterNodeUiMode();
    else enterRadioIdleMode();
    return;
  }

  if (!meshCoreActive) return;

  // PROFILES bar → saved-profile manager.
  if (contentY >= NODE_PROFILES_TOP && contentY <= NODE_PROFILES_TOP + NODE_PROFILES_H) {
    profilesView = true;
    activeScroll() = 0;
    redraw = true;
    return;
  }

  // Worker cards → per-node sub-cluster editor. The nth visible card maps to
  // the nth active node (cards are only drawn for active nodes).
  if (contentY >= NODE_LIST_TOP) {
    const int rel = contentY - NODE_LIST_TOP;
    if (rel % NODE_CARD_PITCH <= NODE_CARD_H) {
      const int row = rel / NODE_CARD_PITCH;
      int seen = 0;
      for (int i = 0; i < CORE_MAX_NODES; ++i) {
        if (!coreNodes[i].active) continue;
        if (seen == row) {
          selectedNode = i;
          nodeDetail = true;
          activeScroll() = 0;
          redraw = true;
          return;
        }
        seen++;
      }
    }
  }
}

void ScreenUI::handleNodeDetailTap(int16_t x, int16_t contentY) {
  if (contentY >= 4 && contentY <= 30 && x <= 82) {   // back
    nodeDetail = false;
    selectedNode = -1;
    redraw = true;
    return;
  }
  if (selectedNode < 0 || selectedNode >= CORE_MAX_NODES ||
      !coreNodes[selectedNode].active) return;

  // BLINK button (drawn at x160..224, y42..64): identify this physical node.
  if (contentY >= 42 && contentY <= 64 && x >= 160 && x <= 224) {
    coreIdentifyNode(selectedNode);
    redraw = true;
    return;
  }

  // UPDATE / CANCEL button (drawn at x160..224, y235..257): push firmware over the air.
  if (contentY >= 233 && contentY <= 259 && x >= 160 && x <= 224) {
    const CoreOtaStatus ota = coreOtaStatus();
    const bool busy = (ota.nodeIdx == selectedNode) &&
                      (ota.phase == CoreOtaPhase::Starting ||
                       ota.phase == CoreOtaPhase::Sending);
    if (busy) coreOtaAbort();
    else      coreOtaStart(selectedNode);
    redraw = true;
    return;
  }

  // Sub-cluster -/+  (buttons drawn at y 122..162).
  if (contentY >= 122 && contentY <= 162) {
    const uint8_t g = coreNodes[selectedNode].group;
    if (x >= 16 && x <= 64) {               // minus
      if (g > 1) coreSetNodeGroup(selectedNode, g - 1);
    } else if (x >= 176 && x <= 224) {      // plus
      if (g < CORE_MAX_GROUPS) coreSetNodeGroup(selectedNode, g + 1);
    }
    redraw = true;
  }
}

void ScreenUI::handleProfilesTap(int16_t x, int16_t contentY) {
  if (contentY >= 4 && contentY <= 30 && x <= 82) {   // back
    profilesView = false;
    redraw = true;
    return;
  }
  // DEFAULT → apply even band-aware split to everyone.
  if (contentY >= 46 && contentY <= 70 && x >= 158 && x <= 226) {
    meshApplyDefault();
    redraw = true;
    return;
  }
  // Custom slot buttons.
  for (uint8_t i = 0; i < MESH_NUM_PROFILES; ++i) {
    const int by = 88 + i * 58 + 28;
    if (contentY < by || contentY > by + 20) continue;
    if (meshProfileUsed(i)) {
      if (x >= 14 && x <= 80)        meshApplyProfile(i);
      else if (x >= 86 && x <= 152)  openProfileNameEditor(i);  // overwrite (rename + recapture)
      else if (x >= 158 && x <= 224) meshDeleteProfile(i);
    } else if (x >= 14 && x <= 80) {
      openProfileNameEditor(i);                                 // save current → new profile
    }
    redraw = true;
    return;
  }
}

void ScreenUI::handleFilesTap(int16_t x, int16_t contentY) {
  if (csvPreview) {
    if (contentY >= 285 && contentY <= 330) {
      csvPreview = false;
      csvRowOffset = 0;
      csvColumns = CsvColumns::Network;
      redraw = true;
    }
    return;
  }
  if (contentY < 48) return;

  const int row = (contentY - 48) / 34;
  if (row >= 0 && row < 6) {
    const size_t index = filesOffset + static_cast<size_t>(row);
    String path, name;
    uint32_t size = 0;
    if (csvFileAt(index, path, name, size)) {
      selectedFile = static_cast<int>(index);
      csvPreview = true;
      csvRowOffset = 0;
      csvColumns = CsvColumns::Network;
      redraw = true;
    }
  }
  if (contentY >= 260 && contentY <= 300) {
    if (x < 72 && filesOffset >= 6) filesOffset -= 6;
    else if (x > 168 && filesOffset + 6 < countCsvFiles()) filesOffset += 6;
    else if (x >= 72 && x <= 168) uploadLogsViaHome();
    redraw = true;
  }
}

void ScreenUI::handleSystemTap(int16_t x, int16_t contentY) {
  if (contentY >= SYS_TOP && contentY < SYS_TOP + 34) {
    speedoMode = true; redraw = true; return;
  }
  int y = SYS_TOP + SYS_TOOLS_H;
  for (int i = 0; i < SETTING_COUNT; ++i) {
    const SettingDef& def = SETTINGS[i];
    const int h = settingHeight(def);
    if (def.kind != SettingKind::Header && contentY >= y && contentY < y + SYS_CARD_H) {
      if (def.kind == SettingKind::Choice) {
        const String current = settingValue(def.key);
        uint8_t next = 0;
        for (uint8_t c = 0; c < def.choiceCount; ++c) {
          if (current == def.choices[c].value) { next = (c + 1) % def.choiceCount; break; }
        }
        cfgAssignKV(def.key, def.choices[next].value);
        persistConfig();
        // A reboot-flagged choice (e.g. CORE PHONE APP) only takes effect on the
        // next boot, and the phone-app bridge is latched at Core entry — so apply
        // it immediately with a reboot, giving a clean single-protocol switch
        // (exactly one of Biscuit / Wardrive Go). Only reboot if the save actually
        // persisted: a failed save in Core mode must NOT reboot and lose the change
        // (the "Last save FAILED" banner then tells the user to set it in solo).
        if (def.reboot && lastSaveOk) {
          frame.fillSprite(C_BG);
          frame.setTextDatum(MC_DATUM);
          frame.setTextColor(C_TEXT, C_BG);
          frame.drawString("APPLYING...", 120, 148, 2);
          frame.setTextColor(C_MUTED, C_BG);
          frame.drawString(def.choices[next].value, 120, 172, 2);
          drawSidePanel();
          present();
          delay(700);
          ESP.restart();
        }
      } else {
        openEditor(i);
      }
      return;
    }
    y += h;
  }
  const int buttonsY = systemButtonsY();
  if (contentY >= buttonsY && contentY < buttonsY + SYS_BUTTON_H) {
    if (x < 120) {
      persistConfig();
    } else {
      frame.fillSprite(C_BG);
      frame.setTextDatum(MC_DATUM);
      frame.setTextColor(C_TEXT, C_BG);
      frame.drawString("REBOOTING", 120, 160, 2);
      drawSidePanel();
      present();
      delay(200);
      ESP.restart();
    }
  }
  redraw = true;
}

void ScreenUI::persistConfig() {
  lastSaveOk = saveConfigToSD();
  themeApplyFromConfig();   // a COLOR THEME change recolors live (no reboot)
  redraw = true;
}

void ScreenUI::openEditor(int settingIndex) {
  editing = true;
  editTarget = EditTarget::Setting;
  editSetting = settingIndex;
  editBuffer = settingValue(SETTINGS[settingIndex].key);
  editLayer = 0;
  redraw = true;
}

void ScreenUI::openProfileNameEditor(int slot) {
  editing = true;
  editTarget = EditTarget::ProfileName;
  editProfileSlot = slot;
  editBuffer = meshProfileUsed(slot) ? String(meshProfileName(slot)) : String("");
  editLayer = 0;
  redraw = true;
}

void ScreenUI::handleEditorTap(int16_t x, int16_t y) {
  const bool profileName = (editTarget == EditTarget::ProfileName);
  const SettingDef& def = SETTINGS[profileName ? 0 : editSetting];
  const bool numeric = !profileName && def.kind == SettingKind::Number;
  redraw = true;

  if (y >= ED_KEYS_Y && y < ED_BOTTOM_Y) {
    const int row = (y - ED_KEYS_Y) / ED_KEY_H;
    const char* keys = numeric ? NUMBER_KEYS[row] : TEXT_KEYS[editLayer][row];
    const int col = x / (SCREEN_W / (numeric ? 3 : 10));
    if (col < static_cast<int>(strlen(keys)) && editBuffer.length() < ED_MAX_LEN) {
      editBuffer += keys[col];
    }
    return;
  }
  if (y < ED_BOTTOM_Y) return;

  switch (min(5, x / 40)) {
    case 0: if (!numeric) editLayer = (editLayer + 1) % 3; break;
    case 1: if (!numeric && editBuffer.length() < ED_MAX_LEN) editBuffer += ' '; break;
    case 2: if (editBuffer.length()) editBuffer.remove(editBuffer.length() - 1); break;
    case 3: editBuffer = ""; break;
    case 4: editing = false; break;
    case 5: {
      String value = editBuffer;
      value.trim();
      if (profileName) {
        meshSaveProfile(editProfileSlot, value.c_str());  // persists to SD itself
        lastSaveOk = true;
        profileSaveTarget = -1;
      } else if (!(numeric && !value.length())) {
        cfgAssignKV(def.key, value);
        persistConfig();
      }
      editing = false;
      break;
    }
  }
}

void ScreenUI::setPage(Page next) {
  if (page == Page::Files && csvPreview && next == Page::Files) {
    csvPreview = false;
    csvRowOffset = 0;
    csvColumns = CsvColumns::Network;
    scroll[static_cast<uint8_t>(Page::Files)] = 0;
    redraw = true;
    return;
  }
  if (page == Page::Files && next != Page::Files) {
    csvPreview = false;
    csvRowOffset = 0;
    csvColumns = CsvColumns::Network;
  }
  if (page == Page::Nodes && next != Page::Nodes) {
    nodeDetail = false;
    profilesView = false;
    selectedNode = -1;
  }
  if (next != Page::Marauder) attackScreen = AttackScreen::Menu;
  page = next;
  redraw = true;
}

bool ScreenUI::webModeActive() const {
  const wifi_mode_t mode = WiFi.getMode();
  return apWindowActive || WiFi.status() == WL_CONNECTED ||
         mode == WIFI_AP || mode == WIFI_AP_STA;
}

void ScreenUI::setWardMode() {
  if (meshCoreActive) exitCoreMode();
  if (meshNodeActive) exitNodeMode();
  if (radioIdle) { WiFi.mode(WIFI_STA); delay(50); radioIdle = false; }
  currentPage = 0;
  scanningEnabled = true;
  userScanOverride = true;
  statusPagePaused = false;
  if (apWindowActive) { apForceClose = true; stopAPIfAllowed(); }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(false);
    WiFi.persistent(false);
    WiFi.disconnect(true, false);
    delay(50);
    WiFi.mode(WIFI_STA);
  }
  redraw = true;
}

void ScreenUI::setWebMode() {
  if (meshCoreActive) exitCoreMode();
  if (meshNodeActive) exitNodeMode();
  if (radioIdle) { WiFi.mode(WIFI_STA); delay(50); radioIdle = false; }
  currentPage = 0;
  scanningEnabled = false;
  userScanOverride = true;
  if (!apWindowActive && WiFi.status() != WL_CONNECTED) startAP();
  redraw = true;
}

void ScreenUI::enterSoloMode() {
  if (meshCoreActive) exitCoreMode();
  if (meshNodeActive) exitNodeMode();
  if (radioIdle) { WiFi.mode(WIFI_STA); delay(50); radioIdle = false; }
  currentPage = 0;
  scanningEnabled = true;
  userScanOverride = true;
  redraw = true;
}

void ScreenUI::enterCoreUiMode() {
  if (meshNodeActive) exitNodeMode();
  radioIdle = false;
  if (!meshCoreActive) enterCoreMode();
  currentPage = 5;
  redraw = true;
}

void ScreenUI::enterNodeUiMode() {
  if (meshCoreActive) exitCoreMode();
  radioIdle = false;
  if (!meshNodeActive) enterNodeMode();
  currentPage = 5;
  redraw = true;
}

void ScreenUI::enterRadioIdleMode() {
  if (meshCoreActive) exitCoreMode();
  if (meshNodeActive) exitNodeMode();
  biscuitReporterReset();
  bleScannerShutdown();

  scanningEnabled = false;
  userScanOverride = true;
  statusPagePaused = true;

  if (apWindowActive) { apForceClose = true; stopAPIfAllowed(); }
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(50);
  WiFi.mode(WIFI_OFF);

  radioIdle = true;
  currentPage = 0;
  redraw = true;
  Serial.println("[MODE] OFF (radio free): scan/BLE/mesh down, Wi-Fi idle");
}

int& ScreenUI::activeScroll() {
  return scroll[static_cast<uint8_t>(page)];
}

int ScreenUI::contentHeightForPage() const {
  switch (page) {
    case Page::Home: return 609;
    case Page::Nodes: {
      if (meshCoreActive) {
        if (nodeDetail || profilesView) return CONTENT_H;  // sub-views don't scroll
        int active = 0;
        for (int i = 0; i < CORE_MAX_NODES; ++i)
          if (coreNodes[i].active) active++;
        return NODE_LIST_TOP + max(1, active) * NODE_CARD_PITCH + 10;
      }
      return 300;
    }
    case Page::Files: return csvPreview ? 340 : 330;
    case Page::System: return systemButtonsY() + SYS_BUTTON_H + 10;
    case Page::Marauder:
      if (attackScreen == AttackScreen::Menu) {
        return 6 + 20 + MARAUDER_ROWS * 32 + 8;
      }
      return CONTENT_H;
    case Page::Tools: return 1080;
  }
  return CONTENT_H;
}

int ScreenUI::maxScroll() const {
  return max(0, contentHeightForPage() - CONTENT_H);
}

void ScreenUI::setScroll(int value) {
  activeScroll() = constrain(value, 0, maxScroll());
}

void ScreenUI::draw() {
  frame.fillSprite(C_BG);
  frame.setTextSize(1);

  // drawSidePanel() is the W550 landscape panel (no-op on the 240x320 CYD). It runs
  // after each mode's draw because the full-screen modes start with fillSprite().
  if (editing) { drawEditor(); drawSidePanel(); present(); return; }
  if (speedoMode) { drawSpeedo(); drawSidePanel(); present(); return; }

  drawHeader();

  frame.setViewport(0, HEADER_H, SCREEN_W, CONTENT_H);
  frame.fillRect(0, 0, SCREEN_W, CONTENT_H, C_BG);

  switch (page) {
    case Page::Home: drawHome(); break;
    case Page::Nodes: drawNodes(); break;
    case Page::Files: csvPreview ? drawCsvPreview() : drawFiles(); break;
    case Page::System: drawSystem(); break;
    case Page::Marauder: drawMarauder(); break;
  }

  drawScrollbar(contentHeightForPage());
  frame.resetViewport();
  drawFooter();
  drawSidePanel();
  present();
}
// Scalable pig face centered at (cx,cy) with head radius R.
void ScreenUI::drawPigFace(int cx, int cy, int R) {
  const uint16_t pink  = rgb565(244, 114, 182);
  const uint16_t lpink = rgb565(251, 182, 206);
  auto S = [&](float v) { return (int)lroundf(v * R / 11.0f); };
  const int er = max(1, S(1.4f));
  const int nr = max(1, S(1.2f));

  frame.fillTriangle(cx + S(-9), cy + S(-7), cx + S(-5), cy + S(-14), cx + S(-1), cy + S(-6), pink);
  frame.fillTriangle(cx + S(1),  cy + S(-7), cx + S(6),  cy + S(-14), cx + S(9),  cy + S(-5), pink);
  frame.fillCircle(cx, cy, R, pink);
  frame.fillRoundRect(cx + S(-6), cy + S(1), S(13), S(8), S(4), lpink);
  frame.fillCircle(cx + S(-3), cy + S(-1), er, C_BG);
  frame.fillCircle(cx + S(4),  cy + S(-1), er, C_BG);
  frame.fillCircle(cx + S(-2), cy + S(5), nr, C_BG);
  frame.fillCircle(cx + S(3),  cy + S(5), nr, C_BG);
}

void ScreenUI::drawShibaLogo(int x, int y) {
  // Colors
  uint16_t C_HOOD  = rgb565(18, 20, 24);
  uint16_t C_GRAY  = rgb565(65, 70, 76);
  uint16_t C_FUR   = rgb565(245, 132, 28);
  uint16_t C_EAR   = rgb565(255, 165, 55);
  uint16_t C_WHITE = rgb565(255, 245, 225);
  uint16_t C_LENS  = rgb565(38, 48, 58);
  uint16_t C_SHINE = rgb565(135, 150, 160);
  uint16_t C_PINK  = rgb565(245, 85, 110);

  // Ears
  frame.fillTriangle(x + 2, y + 8, x + 3, y + 0, x + 10, y + 5, C_HOOD);
  frame.fillTriangle(x + 14, y + 5, x + 21, y + 0, x + 22, y + 8, C_HOOD);
  frame.fillTriangle(x + 4, y + 6, x + 4, y + 2, x + 9, y + 6, C_EAR);
  frame.fillTriangle(x + 15, y + 6, x + 20, y + 2, x + 20, y + 6, C_EAR);
  frame.fillTriangle(x + 5, y + 5, x + 5, y + 3, x + 7, y + 5, C_WHITE);
  frame.fillTriangle(x + 17, y + 5, x + 19, y + 3, x + 19, y + 5, C_WHITE);

  // Hood silhouette
  frame.fillCircle(x + 12, y + 13, 11, C_HOOD);
  frame.fillTriangle(x + 2, y + 11, x + 12, y + 2, x + 22, y + 11, C_HOOD);
  frame.fillTriangle(x + 3, y + 17, x + 12, y + 23, x + 21, y + 17, C_HOOD);

  // Hood highlights
  frame.drawLine(x + 5, y + 9, x + 11, y + 4, C_GRAY);
  frame.drawLine(x + 13, y + 4, x + 19, y + 9, C_GRAY);
  frame.drawLine(x + 3, y + 13, x + 5, y + 18, C_GRAY);
  frame.drawLine(x + 21, y + 13, x + 19, y + 18, C_GRAY);

  // Orange Shiba face
  frame.fillCircle(x + 12, y + 14, 8, C_FUR);

  // White cheeks and muzzle
  frame.fillCircle(x + 8, y + 17, 4, C_WHITE);
  frame.fillCircle(x + 16, y + 17, 4, C_WHITE);
  frame.fillRoundRect(x + 8, y + 16, 9, 5, 2, C_WHITE);

  // Sunglasses
  frame.fillRoundRect(x + 4, y + 10, 8, 6, 2, C_HOOD);
  frame.fillRoundRect(x + 13, y + 10, 8, 6, 2, C_HOOD);
  frame.fillRoundRect(x + 5, y + 11, 6, 4, 2, C_LENS);
  frame.fillRoundRect(x + 14, y + 11, 6, 4, 2, C_LENS);

  // Glasses bridge
  frame.drawLine(x + 11, y + 11, x + 13, y + 11, C_HOOD);

  // Lens reflections
  frame.drawLine(x + 6, y + 11, x + 5, y + 13, C_SHINE);
  frame.drawLine(x + 15, y + 11, x + 14, y + 13, C_SHINE);

  // Nose
  frame.fillCircle(x + 12, y + 16, 2, C_HOOD);

  // Smile
  frame.drawLine(x + 9, y + 19, x + 11, y + 20, C_HOOD);
  frame.drawLine(x + 11, y + 20, x + 13, y + 20, C_HOOD);
  frame.drawLine(x + 13, y + 20, x + 15, y + 19, C_HOOD);

  // Tongue
  frame.fillRoundRect(x + 11, y + 20, 3, 2, 1, C_PINK);
}

void ScreenUI::drawHeader() {
  fillVGrad(frame, 0, 0, SCREEN_W, HEADER_H, C_CARD_2, C_INPUT);
  frame.drawFastHLine(0, HEADER_H - 1, SCREEN_W, C_BORDER);
  const bool active = scanningEnabled && !autoPaused;
  frame.drawFastHLine(0, HEADER_H - 2, SCREEN_W,
                      lerp565(C_BORDER, C_ACCENT, active ? (0.3f + 0.7f * pulse01(1500)) : 0.0f));
  drawShibaLogo(4, 1);

  frame.setTextColor(C_TEXT, C_INPUT);
  frame.setTextSize(1);
  frame.setCursor(32, 3);
  frame.print("WARHOG");

  const char* mode = uploading ? "UPLOAD" :
              (webModeActive() ? "WEB" :
               (meshCoreActive ? "CORE" :
                (meshNodeActive ? "NODE" : "WARD")));
  const uint16_t modeCol = uploading ? C_WARN :
              (meshCoreActive ? C_ACCENT : (meshNodeActive ? C_BLUE : C_MUTED));
  frame.setTextColor(modeCol, C_INPUT);
  frame.setCursor(32, 14);
  frame.print(mode);

  if (uploading) {
    const float base = millis() / 90.0f;
    for (int k = 0; k < 8; ++k) {
      const float a = base + k * (float)(TWO_PI / 8.0);
      const uint16_t c = lerp565(C_INPUT, C_WARN, k / 7.0f);
      frame.fillCircle(66 + (int)(cosf(a) * 5), 13 + (int)(sinf(a) * 5), 1, c);
    }
  }

  if (batteryMonitorAvailable()) {
    const int pct = batteryPercent();
    const uint16_t color = pct > 40 ? C_ACCENT : (pct > 15 ? C_WARN : C_DANGER);
    frame.drawRoundRect(74, 7, 19, 12, 2, C_MUTED);
    frame.fillRect(93, 10, 2, 6, C_MUTED);
    const int fillW = (15 * pct + 50) / 100;
    if (fillW > 0) frame.fillRect(76, 9, fillW, 8, color);
    frame.setTextColor(C_TEXT, C_INPUT);
    frame.setCursor(99, 9);
    frame.printf("%.2fV", batteryVoltage());
  }

  drawStatusChip(137, "GPS", gpsHasFix, !gpsHasFix && gps.charsProcessed() > 0);
  drawStatusChip(172, "SD", sdOk);
  drawStatusChip(205, "RF", scanningEnabled && !autoPaused, webModeActive());
}

void ScreenUI::drawStatusChip(int x, const char* label, bool ok, bool warn) {
  const uint16_t color = ok ? C_ACCENT : (warn ? C_WARN : C_DANGER);
  frame.fillRoundRect(x, 5, 31, 16, 5, C_CARD);
  frame.drawRoundRect(x, 5, 31, 16, 5, C_BORDER);
  const float p = 0.35f + 0.65f * pulse01(1600);
  frame.fillCircle(x + 6, 13, 3, lerp565(C_CARD, color, 0.22f));
  frame.fillCircle(x + 6, 13, 2, lerp565(C_CARD, color, p));
  frame.setTextColor(ok ? C_TEXT : C_MUTED, C_CARD);
  frame.setTextSize(1);
  frame.setCursor(x + 11, 9);
  frame.print(label);
}

void ScreenUI::drawFooter() {
  fillVGrad(frame, 0, FOOTER_Y, SCREEN_W, FOOTER_H, C_INPUT, C_CARD_2);
  frame.drawFastHLine(0, FOOTER_Y, SCREEN_W, C_BORDER);

  static const Page NAV_PAGES[NAV_SLOTS] = {
    Page::Home, Page::Files, Page::System, Page::Marauder
  };

  for (int i = 0; i < NAV_SLOTS; ++i) {
    const int x = i * NAV_SLOT_W;
    const bool selected = (page == NAV_PAGES[i]);

    if (selected) {
      frame.fillRoundRect(x + 2, FOOTER_Y + 4, NAV_SLOT_W - 4, 25, 6, C_ACCENT);
      const uint16_t top = lerp565(C_ACCENT, C_BLUE, 0.4f);
      for (int j = 2; j < 23; ++j) {
        frame.drawFastHLine(x + 4, FOOTER_Y + 4 + j, NAV_SLOT_W - 8,
                            lerp565(top, C_ACCENT, (float)(j - 2) / 20.0f));
      }
      frame.drawFastHLine(x + 5, FOOTER_Y + 5, NAV_SLOT_W - 10, C_TEXT);
    }

    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(selected ? C_BG : C_MUTED, selected ? C_ACCENT : C_INPUT);
    frame.drawString(NAV_LABELS[i], x + NAV_SLOT_W / 2, FOOTER_Y + 17, 1);
  }

  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::drawCard(int x, int y, int w, int h, const char* title) {
  frame.fillRoundRect(x, y, w, h, 6, C_CARD);
  for (int i = 2; i < h - 2; ++i) {
    frame.drawFastHLine(x + 2, y + i, w - 4, lerp565(C_CARD_2, C_CARD, (float)(i - 2) / (h - 4)));
  }
  frame.drawRoundRect(x, y, w, h, 6, C_BORDER);
  frame.drawFastHLine(x + 6, y + 1, w - 12, C_CARD_2);
  frame.setTextSize(1);
  if (title && title[0]) {
    frame.setTextColor(C_MUTED, C_CARD_2);
    frame.setCursor(x + 7, y + 5);
    frame.print(title);
  }
}

void ScreenUI::drawMetric(int x, int y, int w, const char* label, const String& value, uint16_t color) {
  drawCard(x, y, w, 49, label);
  frame.fillRoundRect(x + 2, y + 3, 3, 43, 1, color);
  frame.setTextColor(color, C_CARD);
  frame.setTextSize(2);
  frame.setCursor(x + 9, y + 23);
  frame.print(value);
}

void ScreenUI::drawScrollbar(int contentHeight) {
  if (contentHeight <= CONTENT_H) return;
  const int trackY = 3;
  const int trackH = CONTENT_H - 6;
  const int thumbH = max(22, trackH * CONTENT_H / contentHeight);
  const int denominator = max(1, contentHeight - CONTENT_H);
  const int thumbY = trackY + (trackH - thumbH) * activeScroll() / denominator;

  frame.fillRoundRect(SCREEN_W - 3, trackY, 2, trackH, 1, C_BORDER);
  frame.fillRoundRect(SCREEN_W - 4, thumbY, 3, thumbH, 1, C_ACCENT);
}

void ScreenUI::drawHome() {
  const int s = activeScroll();
  int y = 6 - s;
  const bool web = webModeActive();

  drawCard(6, y, 228, 44, "RADIO / MESH ROLE");
  {
    static const char* const ROLE_LABELS[5] = {"SOLO", "CORE", "NODE", "WEB", "OFF"};
    const bool sel[5] = {
      !meshCoreActive && !meshNodeActive && !radioIdle && !web,
      meshCoreActive,
      meshNodeActive,
      web && !meshCoreActive && !meshNodeActive && !radioIdle,
      radioIdle,
    };
    for (int i = 0; i < 5; ++i) {
      const int bx = 9 + i * 44;
      frame.fillRoundRect(bx, y + 18, 42, 19, 5, sel[i] ? C_ACCENT : C_INPUT);
      frame.setTextDatum(MC_DATUM);
      frame.setTextColor(sel[i] ? C_BG : C_MUTED, sel[i] ? C_ACCENT : C_INPUT);
      frame.drawString(ROLE_LABELS[i], bx + 21, y + 27, 1);
      frame.setTextDatum(TL_DATUM);
    }
  }
  y += 50;

  drawCard(6, y, 228, 50, "STATUS");
  {
    char l1[44], l2[44];
    if (meshCoreActive) {
      snprintf(l1, sizeof(l1), "CORE  %u worker%s",
               (unsigned)coreNodeCount, coreNodeCount == 1 ? "" : "s");
      snprintf(l2, sizeof(l2), "RX %lu   admin ch %u",
               (unsigned long)coreRecordsRx, (unsigned)JCMK_ESPNOW_CH);
    } else if (meshNodeActive) {
      snprintf(l1, sizeof(l1), "NODE  %s", jcmkHaveCore ? "linked" : "searching");
      if (jcmkHaveCore) {
        snprintf(l2, sizeof(l2), "ch %u-%u   found %lu",
                 JCMK_CHANNELS[jcmkStartIdx], JCMK_CHANNELS[jcmkEndIdx],
                 (unsigned long)jcmkNetworksFound);
      } else {
        snprintf(l2, sizeof(l2), "found %lu   sent %lu",
                 (unsigned long)jcmkNetworksFound, (unsigned long)jcmkSentCount);
      }
    } else if (radioIdle) {
      snprintf(l1, sizeof(l1), "RADIO OFF");
      snprintf(l2, sizeof(l2), "Tap SOLO to resume scanning");
    } else if (web) {
      snprintf(l1, sizeof(l1), "WEB (AP) active");
      snprintf(l2, sizeof(l2), "Connect to configure the device");
    } else {
      snprintf(l1, sizeof(l1), "SOLO wardrive   scan %s", scanningEnabled ? "ON" : "OFF");
      snprintf(l2, sizeof(l2), "%s", cfg.scanMode.c_str());
    }
    const uint16_t mc = meshCoreActive ? C_ACCENT : (meshNodeActive ? C_BLUE : C_TEXT);
    frame.setTextColor(mc, C_CARD);
    frame.setCursor(13, y + 20);
    frame.print(l1);
    frame.setTextColor(C_MUTED, C_CARD);
    frame.setCursor(13, y + 36);
    frame.print(l2);
    if (meshCoreActive) {   // hint: tap to open the cluster view
      frame.setTextDatum(MR_DATUM);
      frame.setTextColor(C_ACCENT, C_CARD);
      frame.drawString("CLUSTER >", 228, y + 10, 1);
      frame.setTextDatum(TL_DATUM);
    }
  }
  y += 56;

  drawCard(6, y, 228, 40, "CAPTURE");
  {
    struct { const char* label; bool on; } chips[3] = {
      {"2.4G", cfg.scan24},
      {"5G",   cfg.scan5},
      {"BLE",  cfg.bluetoothScan},
    };
    for (int i = 0; i < 3; ++i) {
      const int cx = 13 + i * 73;
      const bool on = chips[i].on;
      frame.fillRoundRect(cx, y + 14, 68, 19, 5, on ? C_ACCENT : C_INPUT);
      frame.setTextDatum(MC_DATUM);
      frame.setTextColor(on ? C_BG : C_MUTED, on ? C_ACCENT : C_INPUT);
      frame.drawString(String(chips[i].label) + (on ? " ON" : " OFF"),
                       cx + 34, y + 24, 1);
      frame.setTextDatum(TL_DATUM);
    }
  }
  y += 46;

  drawMetric(6, y, 72, "2.4 GHZ", String(networksFound2G), C_ACCENT);
  drawMetric(84, y, 72, "5 GHZ", String(networksFound5G), C_BLUE);
  drawMetric(162, y, 72, "BLE", cfg.bluetoothScan ? String(devicesFoundBle) : String("OFF"),
             cfg.bluetoothScan ? rgb565(167, 139, 250) : C_MUTED);

  y += 55;
  drawMetric(6, y, 111, "SCANS", String(screenTelemetry.scanBatches), C_TEXT);
  drawMetric(123, y, 111, "CSV ROWS", String(screenTelemetry.loggedRows), C_TEXT);

  y += 55;
  drawCard(6, y, 228, 57, "LATEST AP");
  String ssid = screenTelemetry.ssid[0] ? String(screenTelemetry.ssid) : String("--");
  frame.setTextColor(C_TEXT, C_CARD);
  frame.setTextSize(1);
  frame.setCursor(13, y + 20);
  frame.print(shortText(ssid, 31));
  frame.setTextColor(C_MUTED, C_CARD);
  frame.setCursor(13, y + 35);
  if (screenTelemetry.bssid[0]) {
    frame.printf("CH %d  %d dBm  %s", screenTelemetry.channel, screenTelemetry.rssi,
                 shortText(String(screenTelemetry.bssid), 17).c_str());
  } else {
    frame.print(web ? "Web interface active" : "Scan initializing");
  }

  y += 63;
  drawMetric(6, y, 72, "SATS", String(gps.satellites.isValid() ? gps.satellites.value() : 0), gpsHasFix ? C_ACCENT : C_WARN);
  drawMetric(84, y, 72, "SPEED", String(displayedSpeed, 1), C_TEXT);
  drawMetric(162, y, 72, "FOUND", String(screenTelemetry.lastScanFound), C_TEXT);

  y += 55;
  drawCard(6, y, 228, 59, "SESSION");
  frame.setTextColor(C_TEXT, C_CARD);
  frame.setCursor(13, y + 20);
  frame.printf("SD %s   Scan %s   %s", sdOk ? "OK" : "--",
               scanningEnabled ? "ON" : "OFF", cfg.scanMode.c_str());
  frame.setTextColor(C_MUTED, C_CARD);
  frame.setCursor(13, y + 36);
  frame.print(currentCsvPath.length()
                  ? shortText(pathBasename(currentCsvPath), 31)
                  : String("No active CSV"));

  y += 67;
  drawGpsSection(y);
}

// Helper: fill `out` with a short human list of the channels a node scans,
// e.g. "1,6,11,36-48". Collapses runs of adjacent JCMK_CHANNELS entries.
static void formatNodeChannels(const CoreNodeInfo& nd, char* out, size_t outSize) {
  if (nd.chanCount == 0) { snprintf(out, outSize, "(pending)"); return; }
  size_t o = 0;
  out[0] = '\0';
  int i = 0;
  while (i < nd.chanCount) {
    int runStart = nd.chans[i];
    int j = i;
    // extend run while channels are consecutive entries of the JCMK table
    while (j + 1 < nd.chanCount) {
      // find table index of chans[j] and chans[j+1]
      int a = -1, b = -1;
      for (int k = 0; k < JCMK_NUM_CHANNELS; k++) {
        if (JCMK_CHANNELS[k] == nd.chans[j])     a = k;
        if (JCMK_CHANNELS[k] == nd.chans[j + 1]) b = k;
      }
      if (a >= 0 && b == a + 1) j++; else break;
    }
    int runEnd = nd.chans[j];
    int n = (runStart == runEnd)
          ? snprintf(out + o, outSize - o, "%s%d", o ? "," : "", runStart)
          : snprintf(out + o, outSize - o, "%s%d-%d", o ? "," : "", runStart, runEnd);
    if (n < 0 || (size_t)n >= outSize - o) { break; }
    o += n;
    i = j + 1;
  }
}

#if defined(BOARD_W550)
namespace {
// Recent-AP feed for the side panel. Sampled from screenTelemetry (the latest AP)
// at draw time, so the scan path pays nothing; the ring lives in PSRAM, off the
// internal heap a Core is short of.
struct RecentAp { char ssid[23]; char bssid[18]; int16_t rssi; uint8_t ch; };
constexpr int RECENT_N = 12;
RecentAp* g_recent = nullptr;
int g_recentHead = 0;    // next slot to write; newest entry is head-1
int g_recentCount = 0;

void recentSample() {
  if (!screenTelemetry.bssid[0]) return;
  if (!g_recent) {
    g_recent = (RecentAp*)heap_caps_calloc(RECENT_N, sizeof(RecentAp), MALLOC_CAP_SPIRAM);
    if (!g_recent) return;
  }
  for (int i = 0; i < g_recentCount; ++i) {   // the latest AP repeats across draws
    if (strcmp(g_recent[i].bssid, screenTelemetry.bssid) == 0) {
      g_recent[i].rssi = screenTelemetry.rssi;
      return;
    }
  }
  RecentAp& r = g_recent[g_recentHead];
  strlcpy(r.ssid, screenTelemetry.ssid[0] ? screenTelemetry.ssid : "(hidden)", sizeof(r.ssid));
  strlcpy(r.bssid, screenTelemetry.bssid, sizeof(r.bssid));
  r.rssi = screenTelemetry.rssi;
  r.ch = (uint8_t)screenTelemetry.channel;
  g_recentHead = (g_recentHead + 1) % RECENT_N;
  if (g_recentCount < RECENT_N) g_recentCount++;
}

// Fits a count into a size-2 metric tile (<= 4 chars): 9999, 999k, 9.9M, 99M.
String compactCount(uint32_t v) {
  char b[12];
  if (v < 10000) snprintf(b, sizeof(b), "%lu", (unsigned long)v);
  else if (v < 1000000) snprintf(b, sizeof(b), "%luk", (unsigned long)(v / 1000));
  else if (v < 10000000) snprintf(b, sizeof(b), "%.1fM", v / 1000000.0f);
  else snprintf(b, sizeof(b), "%luM", (unsigned long)(v / 1000000));
  return String(b);
}
}  // namespace
#endif

// W550 landscape panel: the 293x320 canvas strip right of the page column, shown
// on every page. Header strip / four metric tiles / cluster roster (Core) or
// recent APs / 2.4 GHz channel activity / position strip. Display-only: touches
// there are ignored (see handleTouch).
void ScreenUI::drawSidePanel() {
#if defined(BOARD_W550)
  recentSample();
  const uint32_t now = millis();
  frame.setViewport(SIDE_X, 0, SIDE_W, SCREEN_H);
  frame.fillRect(0, 0, SIDE_W, SCREEN_H, C_BG);
  frame.setTextSize(1);
  frame.setTextDatum(TL_DATUM);

  // Header strip, continuous with the page column's header.
  fillVGrad(frame, 0, 0, SIDE_W, HEADER_H, C_CARD_2, C_INPUT);
  frame.drawFastHLine(0, HEADER_H - 1, SIDE_W, C_BORDER);
  frame.setTextColor(C_TEXT, C_INPUT);
  frame.setCursor(10, 9);
  frame.print("LIVE");
  frame.setTextColor(C_MUTED, C_INPUT);
  frame.setCursor(40, 9);
  frame.print(radioIdle ? "radio off" : (meshCoreActive ? "core" : (meshNodeActive ? "node" : "solo")));
  {
    const uint32_t up = now / 1000;
    char t[24];
    if (gps.time.isValid() && gps.time.age() < 5000) {
      snprintf(t, sizeof(t), "%02d:%02d:%02d UTC", gps.time.hour(), gps.time.minute(), gps.time.second());
    } else {
      snprintf(t, sizeof(t), "UP %lu:%02lu:%02lu", (unsigned long)(up / 3600),
               (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
    }
    frame.setTextDatum(MR_DATUM);
    frame.setTextColor(C_TEXT, C_INPUT);
    frame.drawString(t, SIDE_W - 8, 13, 1);
    frame.setTextDatum(TL_DATUM);
  }

  // Metric tiles.
  int y = HEADER_H + 6;
  {
    constexpr int TW = 66, TP = 72;
    const String sats = String(gps.satellites.isValid() ? gps.satellites.value() : 0);
    if (meshCoreActive) {
      drawMetric(6 + 0 * TP, y, TW, "NODES", String(coreNodeCount),
                 coreNodeCount ? C_ACCENT : C_MUTED);
      drawMetric(6 + 1 * TP, y, TW, "RECORDS", compactCount(coreRecordsRx), C_BLUE);
      drawMetric(6 + 2 * TP, y, TW, "SATS", sats, gpsHasFix ? C_ACCENT : C_WARN);
    } else {
      drawMetric(6 + 0 * TP, y, TW, "2.4 GHZ", compactCount(networksFound2G), C_ACCENT);
      drawMetric(6 + 1 * TP, y, TW, "5 GHZ", compactCount(networksFound5G), C_BLUE);
      drawMetric(6 + 2 * TP, y, TW, "BLE",
                 cfg.bluetoothScan ? compactCount(devicesFoundBle) : String("OFF"),
                 cfg.bluetoothScan ? rgb565(167, 139, 250) : C_MUTED);
    }
    drawMetric(6 + 3 * TP, y, TW, cfg.speedUnits == "mph" ? "MPH" : "KM/H",
               String(displayedSpeed, displayedSpeed < 99.95f ? 1 : 0), C_TEXT);
  }
  y += 55;

  // Roster (Core) or recent-AP feed. 12 rows = CORE_MAX_NODES, the full cluster.
  constexpr int SP_LIST_H = 138, SP_ROW = 10;
  if (meshCoreActive) {
    drawCard(6, y, SIDE_W - 12, SP_LIST_H, "CLUSTER");
    int row = 0;
    for (int i = 0; i < CORE_MAX_NODES && row < 12; ++i) {
      const CoreNodeInfo& nd = coreNodes[i];
      if (!nd.active) continue;
      const int ry = y + 18 + row * SP_ROW;
      const bool stale = now - nd.lastHbMs > 10000;   // missed two heartbeats
      char ch[40];
      formatNodeChannels(nd, ch, sizeof(ch));
      frame.setTextColor(stale ? C_WARN : C_ACCENT, C_CARD);
      frame.setCursor(13, ry);
      frame.printf("G%u", (unsigned)nd.group);
      frame.setTextColor(stale ? C_MUTED : C_TEXT, C_CARD);
      frame.setCursor(31, ry);
      frame.printf("%02X:%02X:%02X", nd.mac[3], nd.mac[4], nd.mac[5]);
      frame.setTextDatum(TR_DATUM);
      frame.drawString(compactCount(nd.recordsRx), 122, ry, 1);
      frame.setTextDatum(TL_DATUM);
      frame.setTextColor(C_MUTED, C_CARD);
      frame.setCursor(130, ry);
      frame.print(shortText(String(ch), 25));
      row++;
    }
    if (row == 0) {
      frame.setTextColor(C_MUTED, C_CARD);
      frame.setCursor(13, y + 18);
      frame.printf("Waiting for nodes (admin ch %u)", (unsigned)JCMK_ESPNOW_CH);
    }
  } else {
    drawCard(6, y, SIDE_W - 12, SP_LIST_H, "RECENT APS");
    for (int k = 0; k < g_recentCount; ++k) {
      const RecentAp& r = g_recent[(g_recentHead - 1 - k + RECENT_N) % RECENT_N];
      const int ry = y + 18 + k * SP_ROW;
      frame.setTextColor(k == 0 ? C_TEXT : C_MUTED, C_CARD);
      frame.setCursor(13, ry);
      frame.print(shortText(String(r.ssid), 24));
      frame.setCursor(170, ry);
      frame.printf("ch%u", (unsigned)r.ch);
      frame.setTextDatum(TR_DATUM);
      frame.setTextColor(r.rssi >= -60 ? C_ACCENT : (r.rssi >= -75 ? C_TEXT : C_MUTED), C_CARD);
      frame.drawString(String(r.rssi) + " dBm", SIDE_W - 14, ry, 1);
      frame.setTextDatum(TL_DATUM);
    }
    if (g_recentCount == 0) {
      frame.setTextColor(C_MUTED, C_CARD);
      frame.setCursor(13, y + 18);
      frame.print(radioIdle ? "Radio off" : "No APs yet");
    }
  }
  y += SP_LIST_H + 6;

  // 2.4 GHz channel activity (screenTelemetry.channelHits, channels 1..13).
  drawCard(6, y, SIDE_W - 12, 49, "2.4 GHZ ACTIVITY");
  {
    uint32_t peak = 0;
    for (int c = 1; c <= 13; ++c) peak = max(peak, screenTelemetry.channelHits[c]);
    constexpr int SLOT = 21, BAR = 15, BAR_MAX = 20;
    const int base = y + 38;
    for (int c = 1; c <= 13; ++c) {
      const int bx = 12 + (c - 1) * SLOT;
      const int h = peak ? max(1, (int)(screenTelemetry.channelHits[c] * BAR_MAX / peak)) : 1;
      frame.fillRect(bx, base - h, BAR, h,
                     (c == 1 || c == 6 || c == 11) ? C_ACCENT : lerp565(C_CARD_2, C_ACCENT, 0.5f));
      if (c == 1 || c == 6 || c == 11) {
        frame.setTextDatum(TC_DATUM);
        frame.setTextColor(C_MUTED, C_CARD);
        frame.drawString(String(c), bx + BAR / 2, base + 2, 1);
        frame.setTextDatum(TL_DATUM);
      }
    }
  }

  // Position strip, aligned with the page column's footer.
  fillVGrad(frame, 0, FOOTER_Y, SIDE_W, FOOTER_H, C_INPUT, C_CARD_2);
  frame.drawFastHLine(0, FOOTER_Y, SIDE_W, C_BORDER);
  frame.setTextColor(gpsHasFix ? C_TEXT : C_MUTED, C_INPUT);
  frame.setCursor(10, FOOTER_Y + 7);
  if (gpsHasFix) frame.printf("%.6f, %.6f", gps.location.lat(), gps.location.lng());
  else frame.print(gps.charsProcessed() > 0 ? "GPS: searching for fix" : "GPS: no data");
  frame.setTextColor(C_MUTED, C_INPUT);
  frame.setCursor(10, FOOTER_Y + 20);
  frame.printf("HDOP %.1f   SD %s", gps.hdop.isValid() ? gps.hdop.hdop() : 0.0, sdOk ? "OK" : "--");
  if (currentCsvPath.length()) {
    frame.setTextDatum(TR_DATUM);
    frame.drawString(shortText(pathBasename(currentCsvPath), 22), SIDE_W - 8, FOOTER_Y + 20, 1);
    frame.setTextDatum(TL_DATUM);
  }

  // Seam against the page column.
  frame.drawFastVLine(0, 0, SCREEN_H, C_BORDER);
  frame.resetViewport();
#endif
}

void ScreenUI::drawNodes() {
  // Core-mode sub-views take over the whole page.
  if (meshCoreActive && profilesView) { drawProfilesView(); return; }
  if (meshCoreActive && nodeDetail)   { drawNodeDetail();   return; }

  const int s = activeScroll();
  int y = 8 - s;

  drawCard(8, y, 224, 44, "MESH ROLE");
  const char* labels[] = {"SOLO", "CORE", "NODE", "OFF"};
  const bool selected[4] = {
    !meshCoreActive && !meshNodeActive && !radioIdle,
    meshCoreActive,
    meshNodeActive,
    radioIdle,
  };
  for (int i = 0; i < 4; ++i) {
    const int x = 14 + i * 54;
    frame.fillRoundRect(x, y + 18, 50, 19, 5, selected[i] ? C_ACCENT : C_INPUT);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(selected[i] ? C_BG : C_MUTED, selected[i] ? C_ACCENT : C_INPUT);
    frame.drawString(labels[i], x + 25, y + 28, 1);
  }
  frame.setTextDatum(TL_DATUM);

  y += 52;
  drawMetric(8, y, 108, "CONNECTED", String(coreNodeCount), C_ACCENT);
  drawMetric(124, y, 108, "RECORDS RX", String(coreRecordsRx), C_TEXT);

  if (meshCoreActive) {
    // PROFILES bar — opens the saved-profile manager.
    const int py = NODE_PROFILES_TOP - s;
    frame.fillRoundRect(8, py, 224, NODE_PROFILES_H, 6, C_INPUT);
    frame.drawRoundRect(8, py, 224, NODE_PROFILES_H, 6, C_BORDER);
    frame.setTextSize(1);  // drawMetric above leaves size at 2; shrink the bar text
    frame.setTextDatum(ML_DATUM);
    frame.setTextColor(C_ACCENT, C_INPUT);
    frame.drawString("PROFILES", 18, py + NODE_PROFILES_H / 2, 1);
    frame.setTextColor(C_MUTED, C_INPUT);
    frame.setTextDatum(MR_DATUM);
    frame.drawString("tap to manage", 224, py + NODE_PROFILES_H / 2, 1);
    frame.setTextDatum(TL_DATUM);

    int shown = 0;
    for (int i = 0; i < CORE_MAX_NODES; ++i) {
      if (!coreNodes[i].active) continue;
      const int cardY = NODE_LIST_TOP + shown * NODE_CARD_PITCH - s;
      drawCard(8, cardY, 224, NODE_CARD_H, "WORKER");
      char mac[18];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               coreNodes[i].mac[0], coreNodes[i].mac[1], coreNodes[i].mac[2],
               coreNodes[i].mac[3], coreNodes[i].mac[4], coreNodes[i].mac[5]);
      frame.setTextColor(C_TEXT, C_CARD);
      frame.setCursor(16, cardY + 23);
      frame.print(mac);

      // Sub-cluster tag (top-right), colour-keyed so groups are easy to scan.
      char tag[8];
      snprintf(tag, sizeof(tag), "CL%u", (unsigned)coreNodes[i].group);
      const uint16_t tagBg = coreNodes[i].group == 1 ? C_INPUT : C_ACCENT;
      const uint16_t tagFg = coreNodes[i].group == 1 ? C_MUTED : C_BG;
      frame.fillRoundRect(186, cardY + 6, 40, 16, 4, tagBg);
      frame.setTextDatum(MC_DATUM);
      frame.setTextColor(tagFg, tagBg);
      frame.drawString(tag, 206, cardY + 14, 1);
      frame.setTextDatum(TL_DATUM);

      frame.setTextColor(C_MUTED, C_CARD);
      frame.setCursor(16, cardY + 40);
      char chans[40];
      formatNodeChannels(coreNodes[i], chans, sizeof(chans));
      frame.printf("%s  RX %lu  %lus%s", chans,
                   static_cast<unsigned long>(coreNodes[i].recordsRx),
                   static_cast<unsigned long>((millis() - coreNodes[i].lastHbMs) / 1000),
                   coreNodes[i].isBiscuit ? " B" : "");
      shown++;
    }
    if (!shown) {
      const int cardY = NODE_LIST_TOP - s;
      drawCard(8, cardY, 224, 74, "CORE LISTENING");
      frame.setTextColor(C_MUTED, C_CARD);
      frame.setCursor(16, cardY + 27);
      frame.print("0 workers connected");
      frame.setCursor(16, cardY + 44);
      frame.printf("ESP-NOW admin channel %u", JCMK_ESPNOW_CH);
    }
    return;
  }

  y += 65;

  if (meshNodeActive) {
    drawCard(8, y, 224, 100, "NODE STATUS");
    frame.setTextColor(C_TEXT, C_CARD);
    frame.setCursor(16, y + 25);
    frame.printf("Core: %s", jcmkHaveCore ? "linked" : "searching");
    frame.setCursor(16, y + 44);
    if (jcmkHaveCore) {
      frame.printf("Assigned: %u-%u", JCMK_CHANNELS[jcmkStartIdx], JCMK_CHANNELS[jcmkEndIdx]);
    } else {
      frame.print("Assigned: --");
    }
    frame.setCursor(16, y + 63);
    frame.printf("Found: %lu", static_cast<unsigned long>(jcmkNetworksFound));
    frame.setCursor(16, y + 82);
    frame.printf("Sent: %lu", static_cast<unsigned long>(jcmkSentCount));
  } else {
    drawCard(8, y, 224, 85, "SOLO MODE");
    frame.setTextColor(C_MUTED, C_CARD);
    frame.setCursor(16, y + 27);
    frame.print("Standalone wardrive active");
    frame.setCursor(16, y + 46);
    frame.print("Core and Node modes are ready");
    frame.setCursor(16, y + 63);
    frame.print("Select a role above");
  }
}

// Shared back bar for the Core-mode sub-views; returns nothing (fixed layout).
static void drawSubViewBackBar(GfxSprite& frame, const char* title) {
  frame.fillRoundRect(8, 4, 74, 26, 6, C_INPUT);
  frame.drawRoundRect(8, 4, 74, 26, 6, C_BORDER);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_ACCENT, C_INPUT);
  frame.drawString("< BACK", 45, 17, 1);
  frame.setTextColor(C_TEXT, C_BG);
  frame.drawString(title, 158, 17, 2);
  frame.setTextDatum(TL_DATUM);
}

// Per-node editor: shows the node and lets the user move it between
// sub-clusters with  [-] CL n [+] . The Core re-splits channels immediately.
void ScreenUI::drawNodeDetail() {
  drawSubViewBackBar(frame, "NODE");

  if (selectedNode < 0 || selectedNode >= CORE_MAX_NODES ||
      !coreNodes[selectedNode].active) {
    drawCard(8, 40, 224, 60, "NODE");
    frame.setTextColor(C_MUTED, C_CARD);
    frame.setCursor(16, 40 + 34);
    frame.print("Node disconnected");
    return;
  }

  const CoreNodeInfo& nd = coreNodes[selectedNode];

  // Identity card.
  drawCard(8, 36, 224, 60, "WORKER");
  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           nd.mac[0], nd.mac[1], nd.mac[2], nd.mac[3], nd.mac[4], nd.mac[5]);
  frame.setTextColor(C_TEXT, C_CARD);
  frame.setCursor(16, 36 + 24);
  frame.print(mac);
  frame.setTextColor(C_MUTED, C_CARD);
  frame.setCursor(16, 36 + 44);
  frame.printf("%s  RX %lu  %lus",
               nd.band24Only ? "2.4 GHz" : "2.4 + 5 GHz",
               static_cast<unsigned long>(nd.recordsRx),
               static_cast<unsigned long>((millis() - nd.lastHbMs) / 1000));

  // BLINK button (top-right of the identity card): makes this node flash its LED
  // so the operator can tell which physical board this roster entry is.
  frame.fillRoundRect(160, 42, 64, 22, 5, C_ACCENT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString("BLINK", 192, 53, 1);
  frame.setTextDatum(TL_DATUM);

  // Sub-cluster selector.
  drawCard(8, 100, 224, 74, "SUB-CLUSTER");
  frame.fillRoundRect(16, 122, 48, 40, 6, C_INPUT);
  frame.fillRoundRect(176, 122, 48, 40, 6, C_INPUT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_ACCENT, C_INPUT);
  frame.drawString("-", 40, 142, 2);
  frame.drawString("+", 200, 142, 2);
  char cl[8];
  snprintf(cl, sizeof(cl), "CL %u", (unsigned)nd.group);
  frame.setTextColor(C_TEXT, C_CARD);
  frame.setTextSize(2);
  frame.drawString(cl, 120, 140, 1);
  frame.setTextSize(1);
  frame.setTextColor(C_MUTED, C_CARD);
  frame.drawString(String(coreGroupMemberCount(nd.group)) + " node(s) in this cluster",
                   120, 165, 1);
  frame.setTextDatum(TL_DATUM);

  // Channels this node is currently scanning.
  drawCard(8, 178, 224, 50, "SCANNING");
  char chans[40];
  formatNodeChannels(nd, chans, sizeof(chans));
  frame.setTextColor(C_ACCENT, C_CARD);
  frame.setCursor(16, 178 + 30);
  frame.print(chans);

  // Firmware update card: pushes the node image staged at boot from
  // /fw/warhognode_c5.ota.bin over ESP-NOW (see MeshNode.cpp coreOta*). Live progress.
  const CoreOtaStatus ota = coreOtaStatus();
  const bool mine = (ota.nodeIdx == selectedNode) && ota.phase != CoreOtaPhase::Idle;
  const bool busy = mine && (ota.phase == CoreOtaPhase::Starting ||
                             ota.phase == CoreOtaPhase::Sending);
  drawCard(8, 232, 224, 28, "");
  char line[34];
  uint16_t lineCol = C_MUTED;
  if (!mine) {
    if (ota.imageBytes) snprintf(line, sizeof(line), "Image ready: %lu KB", (unsigned long)(ota.imageBytes / 1024));
    else                snprintf(line, sizeof(line), "%s", ota.loadMsg[0] ? ota.loadMsg : "No node image");
  } else if (ota.phase == CoreOtaPhase::Starting) {
    snprintf(line, sizeof(line), "Contacting node...");
    lineCol = C_TEXT;
  } else if (ota.phase == CoreOtaPhase::Sending) {
    unsigned pct = ota.totalBytes ? (unsigned)((uint64_t)ota.sentBytes * 100u / ota.totalBytes) : 0u;
    snprintf(line, sizeof(line), "UPDATING %u%%", pct);
    lineCol = C_ACCENT;
  } else {
    snprintf(line, sizeof(line), "%s", ota.msg);
    lineCol = (ota.phase == CoreOtaPhase::Done) ? C_ACCENT : C_DANGER;
  }
  frame.setTextColor(lineCol, C_CARD);
  frame.setCursor(14, 242);
  frame.print(line);
  const uint16_t btnCol = busy ? C_DANGER : C_ACCENT;
  frame.fillRoundRect(160, 235, 64, 22, 5, btnCol);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, btnCol);
  frame.drawString(busy ? "CANCEL" : "UPDATE", 192, 246, 1);
  frame.setTextDatum(TL_DATUM);
}

// Saved wardriving profiles: a built-in DEFAULT plus MESH_NUM_PROFILES slots.
void ScreenUI::drawProfilesView() {
  drawSubViewBackBar(frame, "PROFILES");

  // DEFAULT.
  drawCard(8, 36, 224, 46, "DEFAULT");
  frame.setTextColor(C_MUTED, C_CARD);
  frame.setCursor(16, 36 + 30);
  frame.print("Even split - band-aware");
  frame.fillRoundRect(158, 46, 68, 24, 5, C_ACCENT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString("APPLY", 192, 58, 1);
  frame.setTextDatum(TL_DATUM);

  for (uint8_t i = 0; i < MESH_NUM_PROFILES; ++i) {
    const int cardY = 88 + i * 58;
    const bool used = meshProfileUsed(i);
    char title[20];
    snprintf(title, sizeof(title), "SLOT %u", (unsigned)(i + 1));
    drawCard(8, cardY, 224, 52, title);

    frame.setTextColor(used ? C_TEXT : C_MUTED, C_CARD);
    frame.setCursor(72, cardY + 6);
    if (used) {
      frame.printf("%s (%u)", meshProfileName(i), (unsigned)meshProfileNodeCount(i));
    } else {
      frame.print("empty");
    }

    // Buttons row.
    const int by = cardY + 28;
    frame.setTextDatum(MC_DATUM);
    if (used) {
      frame.fillRoundRect(14, by, 66, 20, 4, C_ACCENT);
      frame.setTextColor(C_BG, C_ACCENT);
      frame.drawString("LOAD", 47, by + 10, 1);
      frame.fillRoundRect(86, by, 66, 20, 4, C_INPUT);
      frame.setTextColor(C_TEXT, C_INPUT);
      frame.drawString("SAVE", 119, by + 10, 1);
      frame.fillRoundRect(158, by, 66, 20, 4, C_INPUT);
      frame.setTextColor(C_DANGER, C_INPUT);
      frame.drawString("DELETE", 191, by + 10, 1);
    } else {
      frame.fillRoundRect(14, by, 66, 20, 4, C_INPUT);
      frame.setTextColor(C_TEXT, C_INPUT);
      frame.drawString("SAVE", 47, by + 10, 1);
    }
    frame.setTextDatum(TL_DATUM);
  }
}

void ScreenUI::drawGpsSection(int y) {
  drawMetric(6, y, 111, "FIX", gpsHasFix ? "LOCK" : (gps.charsProcessed() ? "WAIT" : "NO DATA"), gpsHasFix ? C_ACCENT : C_WARN);
  drawMetric(123, y, 111, "HEADING",
             [] {
               const double h = headingSmoothedDeg();
               return isfinite(h) ? String(heading8(h)) + " " + String(h, 0) : String("--");
             }(),
             C_TEXT);

  y += 55;
  drawCard(6, y, 228, 96, "GPS POSITION");
  frame.setTextColor(C_TEXT, C_CARD);
  frame.setCursor(14, y + 22);
  frame.printf("Lat  %.6f", gpsHasFix ? gps.location.lat() : lastLat);
  frame.setCursor(14, y + 40);
  frame.printf("Lon  %.6f", gpsHasFix ? gps.location.lng() : lastLon);
  frame.setCursor(14, y + 58);
  frame.printf("Alt  %.1f m   HDOP %.1f",
               gps.altitude.isValid() ? gps.altitude.meters() : lastAlt,
               gps.hdop.isValid() ? gps.hdop.hdop() : lastAcc);
  frame.setTextColor(C_MUTED, C_CARD);
  frame.setCursor(14, y + 76);
  frame.printf("UTC %s", iso8601NowUTC().c_str());
}

size_t ScreenUI::countCsvFiles() const {
  if (!sdOk) return 0;
  size_t count = 0;
  const char* dirs[] = {"/logs", "/uploaded"};

  for (const char* dir : dirs) {
    File root = SD.open(dir);
    if (!root || !root.isDirectory()) continue;
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory() && String(file.name()).endsWith(".csv")) count++;
      file.close();
      file = root.openNextFile();
    }
    root.close();
  }
  return count;
}

bool ScreenUI::csvFileAt(size_t target, String& path, String& name, uint32_t& size) const {
  if (!sdOk) return false;
  size_t index = 0;
  const char* dirs[] = {"/logs", "/uploaded"};

  for (const char* dir : dirs) {
    File root = SD.open(dir);
    if (!root || !root.isDirectory()) continue;
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory() && String(file.name()).endsWith(".csv")) {
        if (index == target) {
          name = pathBasename(file.name());
          path = normalizeSdPath(dir, file.name());
          size = static_cast<uint32_t>(file.size());
          file.close();
          root.close();
          return true;
        }
        index++;
      }
      file.close();
      file = root.openNextFile();
    }
    root.close();
  }
  return false;
}

static bool parseCsv(const String& line, String* fields, int maxFields) {
  int field = 0;
  bool quoted = false;
  String value;

  for (size_t i = 0; i < line.length() && field < maxFields; ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < line.length() && line[i + 1] == '"') {
          value += '"';
          ++i;
        } else {
          quoted = false;
        }
      } else value += c;
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      fields[field++] = value;
      value = "";
    } else value += c;
  }
  if (field < maxFields) fields[field++] = value;
  return field >= 9;
}

bool ScreenUI::readCsvRecord(const String& path, size_t target, String& ssid, String& bssid,
                             String& auth, String& firstSeen, int& channel, int& rssi,
                             double& lat, double& lon) const {
  SdReader file(path);                 // DMA-safe read (see SDUtils.h)
  if (!file) return false;

  file.readStringUntil('\n');
  file.readStringUntil('\n');

  size_t row = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (row++ != target) continue;

    String fields[14];
    const bool ok = parseCsv(line, fields, 14);
    if (ok) {
      bssid = fields[0];
      ssid = fields[1];
      auth = fields[2];
      firstSeen = fields[3];
      channel = fields[4].toInt();
      rssi = fields[6].toInt();
      lat = fields[7].toDouble();
      lon = fields[8].toDouble();
    }
    file.close();
    return ok;
  }

  file.close();
  return false;
}

void ScreenUI::drawFiles() {
  const int s = activeScroll();
  int y = 8 - s;

  drawMetric(8, y, 108, "CSV FILES", String(countCsvFiles()), sdOk ? C_ACCENT : C_DANGER);
  drawMetric(124, y, 108, "SD", sdOk ? "READY" : "NO CARD", sdOk ? C_ACCENT : C_DANGER);
  y += 65;

  for (int row = 0; row < 6; ++row) {
    String path, name;
    uint32_t size = 0;
    const size_t index = filesOffset + row;

    drawCard(8, y, 224, 28, "");
    if (csvFileAt(index, path, name, size)) {
      frame.setTextColor(C_TEXT, C_CARD);
      frame.setCursor(14, y + 9);
      frame.print(shortText(name, 25));
      frame.setTextColor(C_MUTED, C_CARD);
      frame.setCursor(190, y + 9);
      frame.printf("%luK", static_cast<unsigned long>((size + 1023) / 1024));
    } else {
      frame.setTextColor(C_MUTED, C_CARD);
      frame.setCursor(14, y + 9);
      frame.print("--");
    }
    y += 34;
  }

  frame.fillRoundRect(8, y + 3, 60, 28, 6, C_CARD_2);
  frame.fillRoundRect(172, y + 3, 60, 28, 6, C_CARD_2);
  frame.fillRoundRect(74, y + 3, 92, 28, 6, sdOk ? C_ACCENT : C_CARD_2);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_TEXT, C_CARD_2);
  frame.drawString("PREV", 38, y + 17, 1);
  frame.drawString("NEXT", 202, y + 17, 1);
  frame.setTextColor(sdOk ? C_BG : C_MUTED, sdOk ? C_ACCENT : C_CARD_2);
  frame.drawString("UPLOAD HOME", 120, y + 17, 1);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::drawCsvPreview() {
  const int s = activeScroll();
  int y = 8 - s;

  String path, name;
  uint32_t size = 0;
  if (!csvFileAt(static_cast<size_t>(selectedFile), path, name, size)) {
    csvPreview = false;
    drawFiles();
    return;
  }

  drawCard(8, y, 224, 47, "CSV VIEW");
  frame.setTextColor(C_TEXT, C_CARD);
  frame.setCursor(16, y + 24);
  frame.print(shortText(name, 30));
  frame.setTextColor(C_MUTED, C_CARD);
  frame.setCursor(16, y + 36);
  frame.printf("Row %u+   swipe vertical / horizontal", static_cast<unsigned>(csvRowOffset));
  y += 55;

  drawCard(8, y, 224, 25, "");
  frame.setTextColor(C_ACCENT, C_CARD);
  frame.setCursor(14, y + 9);
  if (csvColumns == CsvColumns::Network) frame.print("SSID                   CH RSSI");
  else if (csvColumns == CsvColumns::Security) frame.print("BSSID             AUTH");
  else frame.print("TIME / GPS");
  y += 31;

  for (int row = 0; row < 4; ++row) {
    String ssid, bssid, auth, firstSeen;
    int channel = 0, rssi = 0;
    double lat = 0, lon = 0;
    const bool ok = readCsvRecord(path, csvRowOffset + row, ssid, bssid, auth, firstSeen, channel, rssi, lat, lon);

    drawCard(8, y, 224, 40, "");
    frame.setTextColor(ok ? C_TEXT : C_MUTED, C_CARD);
    frame.setCursor(14, y + 12);
    if (!ok) {
      frame.print("--");
    } else if (csvColumns == CsvColumns::Network) {
      frame.print(shortText(ssid.length() ? ssid : "<hidden>", 22));
      frame.setCursor(173, y + 12);
      frame.printf("%d", channel);
      frame.setCursor(202, y + 12);
      frame.printf("%d", rssi);
    } else if (csvColumns == CsvColumns::Security) {
      frame.print(shortText(bssid, 18));
      frame.setCursor(143, y + 12);
      frame.print(shortText(auth, 12));
    } else {
      frame.print(shortText(firstSeen, 24));
      frame.setTextColor(C_MUTED, C_CARD);
      frame.setCursor(14, y + 26);
      if (lat != 0 || lon != 0) frame.printf("%.4f  %.4f", lat, lon);
      else frame.print("No GPS position");
    }
    y += 46;
  }

  frame.fillRoundRect(70, y + 4, 100, 28, 6, C_CARD_2);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_TEXT, C_CARD_2);
  frame.drawString("BACK", 120, y + 18, 1);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::drawSystem() {
  const int s = activeScroll();
  int y = SYS_TOP - s;
  y = drawSystemTools(y);

  for (const SettingDef& def : SETTINGS) {
    if (def.kind == SettingKind::Header) {
      frame.setTextColor(C_BLUE, C_BG);
      frame.setCursor(10, y + 8);
      frame.print(def.label);
      frame.drawFastHLine(10 + strlen(def.label) * 6 + 6, y + 11, 222 - strlen(def.label) * 6 - 6, C_BORDER);
      y += SYS_HEADER_H;
      continue;
    }
    drawCard(8, y, 224, SYS_CARD_H, def.label);
    if (def.reboot) {
      frame.setTextColor(C_WARN, C_CARD);
      frame.setCursor(189, y + 5);
      frame.print("REBOOT");
    }
    const String value = settingValue(def.key);
    const bool unset = (def.kind == SettingKind::Text || def.kind == SettingKind::Secret) && !value.length();
    frame.setTextColor(unset ? C_MUTED : C_ACCENT, C_CARD);
    frame.setCursor(15, y + 19);
    frame.print(settingDisplay(def));
    frame.setTextColor(C_MUTED, C_CARD);
    frame.setCursor(220, y + 19);
    frame.print(">");
    y += SYS_ROW_H;
  }

  y += 4;
  drawCard(8, y, 224, SYS_INFO_H, "DEVICE");
  frame.setTextColor(C_TEXT, C_CARD);
  frame.setCursor(16, y + 20);
  frame.printf("Firmware: %s", FIRMWARE_VERSION);
  frame.setCursor(16, y + 34);
  frame.print("Changes save to /wardriver.cfg");
  frame.setTextColor(!sdOk ? C_DANGER : (lastSaveOk ? C_MUTED : C_DANGER), C_CARD);
  frame.setCursor(16, y + 48);
  frame.print(!sdOk ? "No SD card - changes not saved" :
              (lastSaveOk ? "Tap a row to change it" : "Last save FAILED"));
  y += SYS_INFO_H + 6;

  frame.fillRoundRect(8, y, 108, 35, 7, C_ACCENT);
  frame.fillRoundRect(124, y, 108, 35, 7, C_DANGER);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString("SAVE", 62, y + 18, 1);
  frame.setTextColor(C_BG, C_DANGER);
  frame.drawString("REBOOT", 178, y + 18, 1);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::triggerHomeUpload() {
  uploadLogsViaHome();
}

void ScreenUI::triggerCoreMode() {
  if (meshCoreActive) enterSoloMode();
  else enterCoreUiMode();
}

void ScreenUI::triggerRadioIdle() {
  if (radioIdle) enterSoloMode();
  else enterRadioIdleMode();
}

void ScreenUI::uploadLogsViaHome() {
  auto banner = [&](const char* l1, const String& l2, uint16_t col) {
    frame.fillSprite(C_BG);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(col, C_BG);
    frame.drawString(l1, 120, 148, 4);
    if (l2.length()) {
      frame.setTextColor(C_MUTED, C_BG);
      frame.drawString(l2, 120, 178, 2);
    }
    drawSidePanel();
    present();
    frame.setTextDatum(TL_DATUM);
  };

  if (!sdOk) { banner("NO SD CARD", "", C_DANGER); delay(1500); redraw = true; return; }
  if (cfg.homeSsid.length() == 0) { banner("NO HOME WIFI", "Set homeSsid first", C_WARN); delay(2000); redraw = true; return; }

  const bool hasWigle = cfg.wigleBasicToken.length() > 0;
  const bool hasWdg   = cfg.wdgwarsApiKey.length() >= 8;
  if (!hasWigle && !hasWdg) { banner("NO API KEYS", "Set WiGLE / WDG key", C_WARN); delay(2000); redraw = true; return; }

  scanningEnabled = false;
  banner("CONNECTING", cfg.homeSsid, C_ACCENT);

  if (!connectSTA(15000)) {
    banner("WIFI FAILED", cfg.homeSsid, C_DANGER);
    delay(2200);
    redraw = true;
    return;
  }

  banner("UPLOADING", "Do not power off", C_ACCENT);
  uint32_t sent = 0;
  if (hasWdg)   sent += uploadAllCsvsToWdgwars(-1);
  if (hasWigle) sent += uploadAllCsvsToWigle(-1);

  banner("UPLOAD DONE", "Sent " + String(sent) + " file(s)", C_ACCENT);
  delay(2500);
  redraw = true;
}

int ScreenUI::drawStats(int y) {
  const uint16_t C_BLE = rgb565(167, 139, 250);

  const uint32_t wifiTotal = networksFound2G + networksFound5G;
  drawMetric(6, y, 111, "WIFI ROWS", String(wifiTotal), C_ACCENT);
  drawMetric(123, y, 111, "BLE ROWS", cfg.bluetoothScan ? String(devicesFoundBle) : String("OFF"),
             cfg.bluetoothScan ? C_BLE : C_MUTED);
  y += 55;

  const int curveH = 104;
  drawCard(6, y, 228, curveH, "DISCOVERY (CUMULATIVE)");
  const int px0 = 14;
  const int py0 = y + 20;
  const int pw = 212;
  const int ph = curveH - 30;
  const int pyBottom = py0 + ph - 1;

  const int n = statsHistoryLen();
  if (n < 2) {
    frame.setTextColor(C_MUTED, C_CARD);
    frame.setCursor(px0, py0 + ph / 2 - 4);
    frame.print("Collecting samples...");
  } else {
    uint32_t maxV = 1;
    for (int i = 0; i < n; ++i) {
      uint32_t w, b;
      statsHistoryGet(i, w, b);
      maxV = max(maxV, max(w, b));
    }
    int prevX = 0, prevYw = 0, prevYb = 0;
    for (int i = 0; i < n; ++i) {
      uint32_t w, b;
      statsHistoryGet(i, w, b);
      const int cx = px0 + (pw - 1) * i / (n - 1);
      const int yw = pyBottom - static_cast<int>((int64_t)(ph - 1) * w / maxV);
      const int yb = pyBottom - static_cast<int>((int64_t)(ph - 1) * b / maxV);
      if (i > 0) {
        frame.drawLine(prevX, prevYw, cx, yw, C_ACCENT);
        if (cfg.bluetoothScan) frame.drawLine(prevX, prevYb, cx, yb, C_BLE);
      }
      prevX = cx; prevYw = yw; prevYb = yb;
    }
    frame.setTextColor(C_ACCENT, C_CARD);
    frame.setCursor(px0, py0 - 1);
    frame.print("WiFi");
    if (cfg.bluetoothScan) {
      frame.setTextColor(C_BLE, C_CARD);
      frame.setCursor(px0 + 34, py0 - 1);
      frame.print("BLE");
    }
    frame.setTextColor(C_MUTED, C_CARD);
    frame.setTextDatum(TR_DATUM);
    frame.drawString("max " + String(maxV), px0 + pw, py0 - 1, 1);
    frame.setTextDatum(TL_DATUM);
  }
  y += curveH + 8;

  const int secRows = SEC_COUNT;
  const int secH = 22 + secRows * 14;
  drawCard(6, y, 228, secH, "SECURITY MIX");
  uint32_t secMax = 1;
  for (int i = 0; i < SEC_COUNT; ++i) secMax = max(secMax, securityTally[i]);

  const uint16_t secColor[SEC_COUNT] = {
    C_DANGER, C_WARN, C_WARN, C_ACCENT, C_BLUE, rgb565(167, 139, 250), C_MUTED
  };
  for (int i = 0; i < SEC_COUNT; ++i) {
    const int rowY = y + 20 + i * 14;
    frame.setTextColor(C_MUTED, C_CARD);
    frame.setCursor(13, rowY + 2);
    frame.print(SEC_LABELS[i]);
    const int barX = 60;
    const int barMaxW = 130;
    const int barW = static_cast<int>((int64_t)barMaxW * securityTally[i] / secMax);
    frame.fillRoundRect(barX, rowY, barMaxW, 9, 2, C_INPUT);
    if (barW > 0) frame.fillRoundRect(barX, rowY, barW, 9, 2, secColor[i]);
    frame.setTextColor(C_TEXT, C_CARD);
    frame.setCursor(barX + barMaxW + 6, rowY + 2);
    frame.print(securityTally[i]);
  }
  y += secH + 8;

  frame.setCursor(10, y + 2);
  if (!cfg.biscuitReport) {
    frame.setTextColor(C_MUTED, C_BG);
    frame.print("Biscuit report: OFF");
  } else {
    const char* st = biscuitLinked ? "LINKED" : (biscuitReporterActive ? "announcing" : "starting");
    frame.setTextColor(biscuitLinked ? C_ACCENT : C_WARN, C_BG);
    frame.printf("Biscuit: %s   sent %lu", st, static_cast<unsigned long>(biscuitRecordsSent));
  }
  return y + 22;
}

void ScreenUI::drawSpeedo() {
  const uint16_t C_BLE = rgb565(167, 139, 250);
  frame.fillSprite(C_BG);

  const bool mph = (cfg.speedUnits == "mph");
  const float spd = displayedSpeed;
  const float maxSpd = 120.0f;
  const float frac = constrain(spd / maxSpd, 0.0f, 1.0f);

  const int cx = 120, cy = 150, r = 112, faceR = 94;
  const float a0 = 150.0f, a1 = 390.0f;
  const int rOut = r, rIn = r - 13;
  const uint16_t tipCol = speedColor(frac);

  frame.fillCircle(cx, cy, faceR, C_CARD);

  const double hdg = headingSmoothedDeg();
  frame.drawCircle(28, 26, 15, C_BORDER);
  frame.fillCircle(28, 26, 14, C_CARD);
  frame.fillCircle(28, 11, 1, C_MUTED);
  if (isfinite(hdg)) {
    const float ha = (float)(hdg - 90.0) * DEG_TO_RAD;
    frame.fillTriangle(28 + (int)(cosf(ha) * 11),        26 + (int)(sinf(ha) * 11),
                       28 + (int)(cosf(ha + 2.5f) * 6),  26 + (int)(sinf(ha + 2.5f) * 6),
                       28 + (int)(cosf(ha - 2.5f) * 6),  26 + (int)(sinf(ha - 2.5f) * 6), C_ACCENT);
  }
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_TEXT, C_BG);
  frame.drawString(isfinite(hdg) ? String(heading8(hdg)) + " " + String((int)lround(hdg))
                                 : String("--"), 134, 22, 4);

  for (int i = 0; i <= 240; ++i) {
    const float t = i / 240.0f;
    const float ang = (a0 + (a1 - a0) * t) * DEG_TO_RAD;
    const float c = cosf(ang), s = sinf(ang);
    if (t <= frac) {
      frame.drawLine(cx + (int)(c * rOut), cy + (int)(s * rOut),
                     cx + (int)(c * rIn),  cy + (int)(s * rIn), speedColor(t));
    } else {
      frame.drawLine(cx + (int)(c * rOut),       cy + (int)(s * rOut),
                     cx + (int)(c * (rOut - 3)), cy + (int)(s * (rOut - 3)), C_BORDER);
    }
  }

  const float angS = a0 * DEG_TO_RAD;
  frame.fillCircle(cx + (int)(cosf(angS) * (rOut - 6)), cy + (int)(sinf(angS) * (rOut - 6)), 6, speedColor(0));
  const float angT = (a0 + (a1 - a0) * frac) * DEG_TO_RAD;
  const int tipx = cx + (int)(cosf(angT) * (rOut - 6)), tipy = cy + (int)(sinf(angT) * (rOut - 6));
  frame.fillCircle(tipx, tipy, 7, tipCol);
  frame.fillCircle(tipx, tipy, 3, C_TEXT);

  for (int k = 0; k <= 8; ++k) {
    const float t = k / 8.0f;
    const float ang = (a0 + (a1 - a0) * t) * DEG_TO_RAD;
    const float c = cosf(ang), s = sinf(ang);
    frame.drawLine(cx + (int)(c * (faceR + 2)), cy + (int)(s * (faceR + 2)),
                   cx + (int)(c * (faceR + 8)), cy + (int)(s * (faceR + 8)),
                   (t <= frac) ? C_MUTED : C_BORDER);
  }

  frame.setTextColor(C_MUTED, C_CARD);
  frame.drawString("0", cx + (int)(cosf(angS) * (r - 26)), cy + (int)(sinf(angS) * (r - 26)), 1);
  frame.drawString(String((int)maxSpd),
                   cx + (int)(cosf(a1 * DEG_TO_RAD) * (r - 26)),
                   cy + (int)(sinf(a1 * DEG_TO_RAD) * (r - 26)), 1);

  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_TEXT, C_CARD);
  frame.setTextSize(4);
  frame.drawString(String((int)lround(spd)), cx, cy - 8, 4);
  frame.setTextSize(1);
  frame.setTextColor(tipCol, C_CARD);
  frame.drawString(mph ? "MPH" : "KM/H", cx, cy + 52, 4);

  const float rpm = radiosPerMinute();
  const float rpmMax = 5000.0f;
  const float rf = constrain(rpm / rpmMax, 0.0f, 1.0f);

  const int ty = 250;
  frame.setTextDatum(ML_DATUM);
  frame.setTextColor(C_MUTED, C_BG);
  frame.drawString("RADIOS/MIN", 10, ty, 2);
  frame.setTextDatum(MR_DATUM);
  frame.setTextColor(speedColor(rf), C_BG);
  frame.drawString(String((int)lroundf(rpm)), 230, ty, 4);

  const int by = ty + 18;
  frame.fillRoundRect(8, by, 224, 14, 6, C_INPUT);
  const int w = (int)(224.0f * rf);
  if (w > 6) frame.fillRoundRect(8, by, w, 14, 6, speedColor(rf));

  const uint32_t apCount = networksFound2G + networksFound5G;
  const uint32_t bleCount = devicesFoundBle;
  frame.setTextDatum(ML_DATUM);
  frame.setTextColor(C_ACCENT, C_BG);
  frame.drawString("AP " + String(apCount), 12, by + 24, 2);
  frame.setTextDatum(MR_DATUM);
  frame.setTextColor(C_BLE, C_BG);
  frame.drawString("BLE " + String(bleCount), 228, by + 24, 2);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::drawEditor() {
  const bool profileName = (editTarget == EditTarget::ProfileName);
  const SettingDef& def = SETTINGS[profileName ? 0 : editSetting];
  const bool numeric = !profileName && def.kind == SettingKind::Number;

  frame.fillRect(0, 0, SCREEN_W, HEADER_H, C_INPUT);
  frame.drawFastHLine(0, HEADER_H - 1, SCREEN_W, C_BORDER);
  frame.setTextSize(1);
  frame.setTextColor(C_TEXT, C_INPUT);
  frame.setCursor(8, 9);
  frame.print(profileName ? "PROFILE NAME" : def.label);
  frame.setTextColor(C_MUTED, C_INPUT);
  frame.setTextDatum(TR_DATUM);
  frame.drawString(String(editBuffer.length()) + "/" + String(ED_MAX_LEN), 232, 9, 1);
  frame.setTextDatum(TL_DATUM);

  frame.fillRoundRect(8, ED_BOX_Y, 224, ED_BOX_H, 6, C_INPUT);
  frame.drawRoundRect(8, ED_BOX_Y, 224, ED_BOX_H, 6, C_ACCENT);
  const String shown = editBuffer + "_";
  const int lines = (shown.length() + ED_CHARS_PER_LINE - 1) / ED_CHARS_PER_LINE;
  const int firstLine = max(0, lines - 4);
  frame.setTextColor(C_TEXT, C_INPUT);
  for (int line = firstLine; line < lines; ++line) {
    frame.setCursor(14, ED_BOX_Y + 8 + (line - firstLine) * 13);
    frame.print(shown.substring(line * ED_CHARS_PER_LINE, (line + 1) * ED_CHARS_PER_LINE));
  }

  frame.setTextColor(C_MUTED, C_BG);
  frame.setCursor(10, ED_HINT_Y);
  if (profileName) frame.print("Name this wardriving profile");
  else if (def.hint) frame.print(def.hint);
  if (!profileName && def.reboot) {
    frame.setTextColor(C_WARN, C_BG);
    frame.setCursor(189, ED_HINT_Y);
    frame.print("REBOOT");
  }

  const int cols = numeric ? 3 : 10;
  const int keyW = SCREEN_W / cols;
  frame.setTextDatum(MC_DATUM);
  for (int row = 0; row < 4; ++row) {
    const char* keys = numeric ? NUMBER_KEYS[row] : TEXT_KEYS[editLayer][row];
    const int ky = ED_KEYS_Y + row * ED_KEY_H;
    for (int col = 0; col < static_cast<int>(strlen(keys)); ++col) {
      const int kx = col * keyW;
      frame.fillRoundRect(kx + 1, ky + 2, keyW - 2, ED_KEY_H - 4, 4, C_CARD_2);
      frame.setTextColor(C_TEXT, C_CARD_2);
      const char label[2] = {keys[col], '\0'};
      frame.drawString(label, kx + keyW / 2, ky + ED_KEY_H / 2, 2);
    }
  }

  for (int i = 0; i < 6; ++i) {
    const int kx = i * 40;
    const bool disabled = numeric && i < 2;
    const uint16_t bg = i == 5 ? C_ACCENT : (i == 4 ? C_DANGER : C_CARD);
    const char* label = i == 0 ? LAYER_NEXT_LABEL[editLayer] : BOTTOM_KEYS[i];
    frame.fillRoundRect(kx + 1, ED_BOTTOM_Y + 2, 38, SCREEN_H - ED_BOTTOM_Y - 4, 4, disabled ? C_INPUT : bg);
    if (!disabled) {
      frame.setTextColor(i >= 4 ? C_BG : C_TEXT, bg);
      frame.drawString(label, kx + 20, ED_BOTTOM_Y + (SCREEN_H - ED_BOTTOM_Y) / 2, 1);
    }
  }
  frame.setTextDatum(TL_DATUM);
}
// The MARAUDER page (own nav tab) hosts the tool menu + per-tool pages.
void ScreenUI::drawMarauder() {
  switch (attackScreen) {
    case AttackScreen::Beacon:   drawBeaconPage(); return;
    case AttackScreen::Deauth:   drawDeauthPage(); return;
    case AttackScreen::Sniff:    drawChannelPage("PROBE SNIFF", AttackMode::ProbeSniff); return;
    case AttackScreen::Capture:  drawCapturePage(); return;
    case AttackScreen::Analyzer: drawWifiAnalyzerPage(); return;
    case AttackScreen::Wps:      drawWpsPage(); return;
    case AttackScreen::Ble:      drawBlePage(); return;
    case AttackScreen::DeauthDet: drawDeauthDetPage(); return;
    case AttackScreen::Pmkid:    drawPmkidPage(); return;
    default: break;
  }
  drawToolsMarauder();
}

// Diagnostics/telemetry block (former TOOLS page) at the top of the SYS page.
int ScreenUI::drawSystemTools(int y) {
  const int y0 = y;

  frame.fillRoundRect(6, y, 228, 34, 8, C_ACCENT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString("FULL-SCREEN SPEEDO", 120, y + 17, 2);
  frame.setTextDatum(TL_DATUM);
  y += 42;

  y = drawStats(y);
  y += 6;

  drawMetric(6, y, 111, "HEAP", String(ESP.getFreeHeap() / 1024) + " KB", C_ACCENT);
  drawMetric(123, y, 111, "PSRAM", String(ESP.getFreePsram() / 1024) + " KB", C_BLUE);
  y += 55;

  drawCard(6, y, 228, 62, "RADIO");
  frame.setTextColor(C_TEXT, C_CARD);
  frame.setCursor(13, y + 20);
  frame.printf("Mode %s   AP clients %u", wifiModeLabel(), WiFi.softAPgetStationNum());
  frame.setCursor(13, y + 36);
  frame.printf("STA RSSI %d dBm", WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  frame.setCursor(13, y + 50);
  frame.printf("Ward %s   Auto pause %s", scanningEnabled ? "ON" : "OFF", autoPaused ? "YES" : "NO");
  y += 68;

  drawCard(6, y, 228, 62, "SCAN TELEMETRY");
  frame.setTextColor(C_TEXT, C_CARD);
  frame.setCursor(13, y + 20);
  frame.printf("Batches %lu   Found %d", static_cast<unsigned long>(screenTelemetry.scanBatches), screenTelemetry.lastScanFound);
  frame.setCursor(13, y + 36);
  frame.printf("Rows %lu   2.4G %lu", static_cast<unsigned long>(screenTelemetry.loggedRows), static_cast<unsigned long>(networksFound2G));
  frame.setCursor(13, y + 50);
  frame.printf("5G %lu   CH %d", static_cast<unsigned long>(networksFound5G), screenTelemetry.channel);
  y += 68;

  drawCard(6, y, 228, 232, "CHANNEL OCCUPANCY");
  uint32_t peak = 1;
  for (int channel = 1; channel <= 13; ++channel) {
    peak = max(peak, screenTelemetry.channelHits[channel]);
  }
  for (int channel = 1; channel <= 13; ++channel) {
    const int rowY = y + 20 + (channel - 1) * 16;
    const int barW = static_cast<int>(151UL * screenTelemetry.channelHits[channel] / peak);
    frame.setTextColor(C_MUTED, C_CARD);
    frame.setCursor(13, rowY + 3);
    frame.printf("%2d", channel);
    frame.fillRoundRect(35, rowY, 157, 10, 3, C_INPUT);
    if (barW > 0) frame.fillRoundRect(35, rowY, barW, 10, 3, C_ACCENT);
    frame.setTextColor(C_TEXT, C_CARD);
    frame.setCursor(199, rowY + 3);
    frame.print(screenTelemetry.channelHits[channel]);
  }

  return y0 + SYS_TOOLS_H;
}

// Marauder tool menu: one scrollable card of rows.
void ScreenUI::drawToolsMarauder() {
  const int s = activeScroll();
  const int CARD_Y = 6 - s;
  const int ROW_H = 32;

  drawCard(6, CARD_Y, 228, 20 + MARAUDER_ROWS * ROW_H, "MARAUDER");

  for (int i = 0; i < MARAUDER_ROWS; ++i) {
    const int rowY = CARD_Y + 20 + i * ROW_H;
    const bool running = marauderRowRunning(i);
    frame.fillRoundRect(12, rowY, 216, ROW_H - 4, 5, running ? C_DANGER : C_CARD_2);
    frame.setTextDatum(ML_DATUM);
    frame.setTextColor(running ? C_BG : C_TEXT, running ? C_DANGER : C_CARD_2);
    frame.drawString(MARAUDER_LABELS[i], 22, rowY + (ROW_H - 4) / 2, 2);

    String right = running ? String("RUNNING") : String(">");
    if (running) {
      switch (static_cast<MRow>(i)) {
        case MRow::AirTag:  right = "ON " + String(airTagMonitorCount());  break;
        case MRow::Flipper: right = "ON " + String(flipperSniffCount());   break;
        case MRow::Pwn:     right = "ON " + String(pwnagotchiDetectCount()); break;
        case MRow::Ble:     right = "ON " + String(bleSpamCount());        break;
        case MRow::Pmkid:   right = String(pmkidCaptures()) + "/" + String(pmkidAttempts()); break;
        default: break;
      }
    }
    frame.setTextDatum(MR_DATUM);
    frame.setTextColor(running ? C_BG : C_MUTED, running ? C_DANGER : C_CARD_2);
    frame.drawString(right, 220, rowY + (ROW_H - 4) / 2, 1);
    frame.setTextDatum(TL_DATUM);
  }
}

// ── Shared tool-page furniture ───────────────────────────────────────────────
namespace {
constexpr int TH_Y  = 6;
constexpr int TH_H  = 30;
constexpr int TH_BODY = TH_Y + TH_H + 8;
}

int ScreenUI::drawToolHeader(const char* title) {
  frame.fillRoundRect(6, TH_Y - activeScroll(), 228, TH_H, 6, C_CARD_2);
  frame.setTextDatum(ML_DATUM);
  frame.setTextColor(C_ACCENT, C_CARD_2);
  frame.drawString("< BACK", 16, TH_Y - activeScroll() + TH_H / 2, 2);
  frame.setTextColor(C_TEXT, C_CARD_2);
  frame.setTextDatum(MR_DATUM);
  frame.drawString(title, 224, TH_Y - activeScroll() + TH_H / 2, 2);
  frame.setTextDatum(TL_DATUM);
  return TH_BODY;
}

bool ScreenUI::toolHeaderBackHit(int16_t contentY) const {
  return contentY >= TH_Y && contentY < TH_Y + TH_H;
}

int ScreenUI::drawRunningStats(int y) {
  const int s = activeScroll();
  const AttackMode cur = marauderCurrentAttack();
  const int statsH = (cur == AttackMode::Deauth) ? 46 : 30;

  frame.fillRoundRect(12, y - s, 216, statsH, 6, C_INPUT);
  frame.setTextDatum(ML_DATUM);
  frame.setTextColor(C_TEXT, C_INPUT);
  char l1[48];
  if (cur == AttackMode::ProbeSniff) {
    snprintf(l1, sizeof(l1), "RX %lu  %lu/s",
             (unsigned long)marauderFramesCaptured(),
             (unsigned long)marauderFramesPerSec());
  } else if (cur == AttackMode::PcapCapture) {
    snprintf(l1, sizeof(l1), "SAVED %lu", (unsigned long)marauderFramesWritten());
  } else {
    snprintf(l1, sizeof(l1), "TX %lu  %lu/s  fail %lu",
             (unsigned long)marauderFramesSent(),
             (unsigned long)marauderFramesPerSec(),
             (unsigned long)marauderFramesFailed());
  }
  frame.drawString(l1, 20, y - s + (cur == AttackMode::Deauth ? 13 : statsH / 2), 2);
  if (cur == AttackMode::Deauth) {
    frame.setTextColor(C_MUTED, C_INPUT);
    frame.drawString(marauderDeauthTargetStr() + "  ch" + String(marauderCurrentChannel()),
                     20, y - s + 34, 1);
  }
  frame.setTextDatum(TL_DATUM);

  const int stopY = y + statsH + 8;
  frame.fillRoundRect(12, stopY - s, 216, 40, 8, C_DANGER);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_DANGER);
  frame.drawString("STOP", 120, stopY - s + 20, 2);
  frame.setTextDatum(TL_DATUM);
  return stopY;
}

int ScreenUI::statsStopY() const {
  const int statsH = (marauderCurrentAttack() == AttackMode::Deauth) ? 46 : 30;
  return (TH_BODY + 8) + statsH + 8;
}

namespace {
constexpr int CG_X0 = 12;
constexpr int CG_W  = 31;
constexpr int CG_H  = 28;
constexpr int CG_COLS = 7;
}

void ScreenUI::drawChannelGrid(int gridTop) {
  for (int ch = 1; ch <= 14; ++ch) {
    const int i = ch - 1;
    const int cx = CG_X0 + (i % CG_COLS) * CG_W;
    const int cy = gridTop + (i / CG_COLS) * CG_H;
    const bool selected = (ch == selectedChannel);
    frame.fillRoundRect(cx, cy, CG_W - 3, CG_H - 3, 5,
                        selected ? C_ACCENT : C_CARD_2);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(selected ? C_BG : C_TEXT,
                       selected ? C_ACCENT : C_CARD_2);
    frame.drawString(String(ch), cx + (CG_W - 3) / 2, cy + (CG_H - 3) / 2, 2);
    frame.setTextDatum(TL_DATUM);
  }
}

int ScreenUI::channelGridHit(int16_t x, int16_t contentY, int gridTop) const {
  if (contentY < gridTop || contentY >= gridTop + 2 * CG_H) return 0;
  const int col = (x - CG_X0) / CG_W;
  if (col < 0 || col >= CG_COLS) return 0;
  const int ch = ((contentY - gridTop) / CG_H) * CG_COLS + col + 1;
  return (ch >= 1 && ch <= 14) ? ch : 0;
}

// ── Beacon-spam tool page ────────────────────────────────────────────────────
namespace {
constexpr int BC_SRC_Y    = 44;
constexpr int BC_CUSTOM_Y = 76;
constexpr int BC_HOP_Y    = 106;
constexpr int BC_GRID_Y   = 136;
constexpr int BC_START_Y  = 198;
constexpr int BC_ROW_H    = 28;
}

void ScreenUI::drawBeaconPage() {
  drawToolHeader("BEACON SPAM");
  const int s = activeScroll();

  if (marauderCurrentAttack() == AttackMode::BeaconSpam) {
    drawRunningStats(TH_BODY + 8);
    return;
  }

  static const char* SRC[3] = {"RICK", "RANDOM", "CUSTOM"};
  for (int i = 0; i < 3; ++i) {
    const int bx = 12 + i * 73;
    const bool sel = (static_cast<int>(beaconSrc) == i);
    frame.fillRoundRect(bx, BC_SRC_Y - s, 69, BC_ROW_H, 5, sel ? C_ACCENT : C_CARD_2);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(sel ? C_BG : C_TEXT, sel ? C_ACCENT : C_CARD_2);
    frame.drawString(SRC[i], bx + 34, BC_SRC_Y - s + BC_ROW_H / 2, 2);
    frame.setTextDatum(TL_DATUM);
  }

  frame.fillRoundRect(12, BC_CUSTOM_Y - s, 156, 26, 5, C_INPUT);
  frame.setTextDatum(ML_DATUM);
  frame.setTextColor(cfg.beaconSsid.length() ? C_TEXT : C_MUTED, C_INPUT);
  frame.drawString(cfg.beaconSsid.length() ? cfg.beaconSsid : "(tap TYPE)",
                   18, BC_CUSTOM_Y - s + 13, 2);
  frame.fillRoundRect(174, BC_CUSTOM_Y - s, 54, 26, 5, C_ACCENT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString("TYPE", 201, BC_CUSTOM_Y - s + 13, 2);
  frame.setTextDatum(TL_DATUM);

  frame.fillRoundRect(12, BC_HOP_Y - s, 216, 26, 5, C_CARD_2);
  frame.setTextDatum(ML_DATUM);
  frame.setTextColor(C_TEXT, C_CARD_2);
  frame.drawString("BROADCAST ON ALL CH", 18, BC_HOP_Y - s + 13, 2);
  const uint16_t pill = beaconHop ? C_ACCENT : C_INPUT;
  frame.fillRoundRect(176, BC_HOP_Y - s + 3, 46, 20, 5, pill);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(beaconHop ? C_BG : C_MUTED, pill);
  frame.drawString(beaconHop ? "ON" : "OFF", 199, BC_HOP_Y - s + 13, 2);
  frame.setTextDatum(TL_DATUM);

  drawChannelGrid(BC_GRID_Y - s);

  frame.fillRoundRect(12, BC_START_Y - s, 216, 40, 8, C_ACCENT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString(beaconHop ? String("START  ALL CH")
                             : ("START  CH " + String(selectedChannel)),
                   120, BC_START_Y - s + 20, 2);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::handleBeaconPageTap(int16_t x, int16_t contentY) {
  if (toolHeaderBackHit(contentY)) { attackScreen = AttackScreen::Menu; redraw = true; return; }

  if (marauderCurrentAttack() == AttackMode::BeaconSpam) {
    const int stopY = statsStopY();
    if (contentY >= stopY && contentY < stopY + 40) { marauderStopAttack(); redraw = true; }
    return;
  }

  if (contentY >= BC_SRC_Y && contentY < BC_SRC_Y + BC_ROW_H) {
    const int i = (x - 12) / 73;
    if (i >= 0 && i <= 2) { beaconSrc = static_cast<BeaconSource>(i); redraw = true; }
    return;
  }
  if (contentY >= BC_CUSTOM_Y && contentY < BC_CUSTOM_Y + 26) {
    if (x >= 174) {
      beaconSrc = BeaconSource::Custom;
      const int idx = settingIndexByKey("beaconSsid");
      if (idx >= 0) openEditor(idx);
    }
    return;
  }
  if (contentY >= BC_HOP_Y && contentY < BC_HOP_Y + 26) {
    beaconHop = !beaconHop;
    redraw = true;
    return;
  }
  const int ch = channelGridHit(x, contentY, BC_GRID_Y);
  if (ch) { selectedChannel = (uint8_t)ch; redraw = true; return; }
  if (contentY >= BC_START_Y && contentY < BC_START_Y + 40) {
    marauderSetBeaconConfig(beaconSrc, cfg.beaconSsid, beaconHop);
    marauderStartAttack(AttackMode::BeaconSpam, selectedChannel);
    redraw = true;
  }
}

// ── Multi-target deauth page ─────────────────────────────────────────────────
void ScreenUI::drawDeauthPage() {
  drawToolHeader("DEAUTH");
  const int s = activeScroll();

  if (marauderCurrentAttack() == AttackMode::Deauth) {
    drawRunningStats(TH_BODY + 8);
    return;
  }

  const size_t selCount = marauderDeauthTargetCount();
  char hdr[40];
  snprintf(hdr, sizeof(hdr), "%u selected / %u APs",
           (unsigned)selCount, (unsigned)marauderScanResultCount());
  frame.setTextColor(C_MUTED, C_BG);
  frame.setCursor(14, TH_BODY - s + 2);
  frame.print(hdr);

  const int LIST_Y = TH_BODY + 18;
  const int VISIBLE = 5;
  const int ROW_H = 32;

  if (marauderScanResultCount() == 0) {
    frame.setTextColor(C_MUTED, C_BG);
    frame.setCursor(14, LIST_Y - s + 8);
    frame.print("No APs found on 2.4 GHz");
  } else {
    for (int r = 0; r < VISIBLE; ++r) {
      const size_t idx = bssidPickerOffset + r;
      String ssid, bssid;
      int ch = 0, rssi = 0;
      if (!marauderScanResultGet(idx, ssid, bssid, ch, rssi)) continue;

      const bool sel = marauderDeauthTargetSelected(bssid);
      const int rowY = LIST_Y - s + r * ROW_H;

      frame.fillRoundRect(12, rowY, 216, ROW_H - 4, 5,
                          sel ? C_ACCENT : C_CARD_2);

      const int cbX = 18, cbY = rowY + (ROW_H - 4 - 14) / 2;
      frame.drawRoundRect(cbX, cbY, 14, 14, 2, sel ? C_BG : C_MUTED);
      if (sel) frame.fillRoundRect(cbX + 3, cbY + 3, 8, 8, 1, C_BG);

      frame.setTextColor(sel ? C_BG : C_TEXT, sel ? C_ACCENT : C_CARD_2);
      frame.setCursor(40, rowY + 5);
      frame.print(shortText(ssid.length() ? ssid : String("<hidden>"), 24));

      frame.setTextColor(sel ? C_BG : C_MUTED, sel ? C_ACCENT : C_CARD_2);
      frame.setCursor(40, rowY + 18);
      frame.print(shortText(bssid, 17));
      frame.setCursor(170, rowY + 18);
      frame.printf("ch%d %d", ch, rssi);
    }
  }

  const int btnY = LIST_Y - s + VISIBLE * ROW_H + 4;

  frame.fillRoundRect(12, btnY, 68, 28, 6, C_CARD_2);
  frame.fillRoundRect(86, btnY, 68, 28, 6, C_CARD_2);
  frame.fillRoundRect(160, btnY, 68, 28, 6, C_CARD_2);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_TEXT, C_CARD_2);
  frame.drawString("UP",    46, btnY + 14, 2);
  frame.drawString("DOWN", 120, btnY + 14, 2);
  frame.drawString("RESCAN", 194, btnY + 14, 1);
  frame.setTextDatum(TL_DATUM);

  const int startY = btnY + 32;
  const bool canStart = selCount > 0;
  frame.fillRoundRect(12, startY, 216, 40, 8, canStart ? C_ACCENT : C_INPUT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(canStart ? C_BG : C_MUTED, canStart ? C_ACCENT : C_INPUT);
  char startLabel[40];
  snprintf(startLabel, sizeof(startLabel), "START  %u TARGET%s",
           (unsigned)selCount, selCount == 1 ? "" : "S");
  frame.drawString(startLabel, 120, startY + 20, 2);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::handleDeauthPageTap(int16_t x, int16_t contentY) {
  if (toolHeaderBackHit(contentY)) { attackScreen = AttackScreen::Menu; redraw = true; return; }

  if (marauderCurrentAttack() == AttackMode::Deauth) {
    const int stopY = statsStopY();
    if (contentY >= stopY && contentY < stopY + 40) { marauderStopAttack(); redraw = true; }
    return;
  }

  const int LIST_Y = TH_BODY + 18;
  const int VISIBLE = 5;
  const int ROW_H = 32;

  if (contentY >= LIST_Y && contentY < LIST_Y + VISIBLE * ROW_H) {
    const size_t idx = bssidPickerOffset + (contentY - LIST_Y) / ROW_H;
    String ssid, bssid;
    int ch = 0, rssi = 0;
    if (marauderScanResultGet(idx, ssid, bssid, ch, rssi)) {
      if (marauderDeauthTargetSelected(bssid)) marauderRemoveDeauthTarget(bssid);
      else                                     marauderAddDeauthTarget(bssid, (uint8_t)ch);
      redraw = true;
    }
    return;
  }

  const int btnY = LIST_Y + VISIBLE * ROW_H + 4;
  if (contentY >= btnY && contentY < btnY + 28) {
    if (x < 80) {
      if (bssidPickerOffset >= VISIBLE) bssidPickerOffset -= VISIBLE;
    } else if (x < 154) {
      if (bssidPickerOffset + VISIBLE < marauderScanResultCount()) bssidPickerOffset += VISIBLE;
    } else {
      marauderRefreshScanResults();
      bssidPickerOffset = 0;
    }
    redraw = true;
    return;
  }

  const int startY = btnY + 32;
  if (contentY >= startY && contentY < startY + 40) {
    if (marauderDeauthTargetCount() > 0) {
      String b; uint8_t ch = 6;
      marauderDeauthTargetGet(0, b, ch);
      marauderStartAttack(AttackMode::Deauth, ch);
      redraw = true;
    }
  }
}

// Shared channel-based tool page (Probe Sniff / PCAP Capture).
void ScreenUI::drawChannelPage(const char* title, AttackMode mode) {
  drawToolHeader(title);

  if (marauderCurrentAttack() == mode) {
    drawRunningStats(TH_BODY + 8);
    return;
  }

  const int s = activeScroll();
  drawChannelGrid(TH_BODY - s);

  const int startY = TH_BODY + 2 * 28 + 10;
  frame.fillRoundRect(12, startY - s, 216, 40, 8, C_ACCENT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString("START  CH " + String(selectedChannel), 120, startY - s + 20, 2);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::handleChannelPageTap(int16_t x, int16_t contentY, AttackMode mode) {
  if (toolHeaderBackHit(contentY)) { attackScreen = AttackScreen::Menu; redraw = true; return; }

  if (marauderCurrentAttack() == mode) {
    const int stopY = statsStopY();
    if (contentY >= stopY && contentY < stopY + 40) { marauderStopAttack(); redraw = true; }
    return;
  }

  const int ch = channelGridHit(x, contentY, TH_BODY);
  if (ch) { selectedChannel = (uint8_t)ch; redraw = true; return; }

  const int startY = TH_BODY + 2 * 28 + 10;
  if (contentY >= startY && contentY < startY + 40) {
    marauderStartAttack(mode, selectedChannel);
    redraw = true;
  }
}

// ── BLE spam page ────────────────────────────────────────────────────────────
namespace {
constexpr int BS_PICK_Y = TH_BODY;
constexpr int BS_ROW_H  = 34;
constexpr int BS_ROW_W  = 108;
constexpr int BS_START_Y = TH_BODY + 2 * 34 + 12;
}

void ScreenUI::drawBlePage() {
  drawToolHeader("BLE SPAM");
  const int s = activeScroll();

  if (bleSpamActive()) {
    const int y = TH_BODY + 8;
    frame.fillRoundRect(12, y - s, 216, 46, 6, C_INPUT);
    frame.setTextDatum(ML_DATUM);
    frame.setTextColor(C_TEXT, C_INPUT);
    frame.drawString(bleSpamTypeName(bleSpamCurrentType()), 20, y - s + 14, 2);
    frame.setTextColor(C_MUTED, C_INPUT);
    frame.drawString("SENT " + String(bleSpamCount()), 20, y - s + 34, 1);
    frame.setTextDatum(TL_DATUM);

    const int stopY = y + 46 + 8;
    frame.fillRoundRect(12, stopY - s, 216, 40, 8, C_DANGER);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(C_BG, C_DANGER);
    frame.drawString("STOP", 120, stopY - s + 20, 2);
    frame.setTextDatum(TL_DATUM);
    return;
  }

  static const BleSpamType TYPES[4] = {
    BleSpamType::Apple, BleSpamType::SwiftPair,
    BleSpamType::Samsung, BleSpamType::FastPair
  };
  for (int i = 0; i < 4; ++i) {
    const int bx = 12 + (i % 2) * 112;
    const int by = BS_PICK_Y - s + (i / 2) * BS_ROW_H;
    const bool sel = (bleType == TYPES[i]);
    frame.fillRoundRect(bx, by, BS_ROW_W, BS_ROW_H - 4, 5,
                        sel ? C_ACCENT : C_CARD_2);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(sel ? C_BG : C_TEXT, sel ? C_ACCENT : C_CARD_2);
    frame.drawString(bleSpamTypeName(TYPES[i]),
                     bx + BS_ROW_W / 2, by + (BS_ROW_H - 4) / 2, 2);
    frame.setTextDatum(TL_DATUM);
  }

  const int dy = BS_PICK_Y - s + 2 * BS_ROW_H + 4;
  frame.setTextColor(C_MUTED, C_BG);
  frame.setCursor(14, dy);
  switch (bleType) {
    case BleSpamType::Apple:     frame.print("iOS / macOS pairing popups");      break;
    case BleSpamType::SwiftPair: frame.print("Windows 10/11 device popups");     break;
    case BleSpamType::Samsung:   frame.print("Samsung watch / buds popups");     break;
    case BleSpamType::FastPair:  frame.print("Android / Google Fast Pair popups"); break;
    default: break;
  }

  frame.fillRoundRect(12, BS_START_Y - s, 216, 40, 8, C_ACCENT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString("START  " + String(bleSpamTypeName(bleType)),
                   120, BS_START_Y - s + 20, 2);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::handleBlePageTap(int16_t x, int16_t contentY) {
  if (toolHeaderBackHit(contentY)) { attackScreen = AttackScreen::Menu; redraw = true; return; }

  if (bleSpamActive()) {
    const int stopY = TH_BODY + 8 + 46 + 8;
    if (contentY >= stopY && contentY < stopY + 40) { bleSpamStop(); redraw = true; }
    return;
  }

  if (contentY >= BS_PICK_Y && contentY < BS_PICK_Y + 2 * BS_ROW_H) {
    const int row = (contentY - BS_PICK_Y) / BS_ROW_H;
    const int col = (x - 12) / 112;
    if (col >= 0 && col < 2) {
      static const BleSpamType TYPES[4] = {
        BleSpamType::Apple, BleSpamType::SwiftPair,
        BleSpamType::Samsung, BleSpamType::FastPair
      };
      const int idx = row * 2 + col;
      if (idx >= 0 && idx < 4) { bleType = TYPES[idx]; redraw = true; }
    }
    return;
  }

  if (contentY >= BS_START_Y && contentY < BS_START_Y + 40) {
    bleSpamStart(bleType);
    redraw = true;
  }
}

// ── Capture tool page ────────────────────────────────────────────────────────
namespace {
const char* const RAW_FILTER_LABELS[5] = {"ALL", "BCN", "PRB", "DEA", "EAP"};
constexpr int CAP_MODE_Y   = TH_BODY;
constexpr int CAP_FILTER_Y = TH_BODY + 32;
constexpr int CAP_GRID_Y   = TH_BODY + 64;
constexpr int CAP_START_Y  = CAP_GRID_Y + 2 * 28 + 10;
}

void ScreenUI::drawCapturePage() {
  drawToolHeader("CAPTURE");
  const int s = activeScroll();
  const AttackMode cur = marauderCurrentAttack();

  if (cur == AttackMode::PcapCapture) { drawRunningStats(TH_BODY + 8); return; }

  if (cur == AttackMode::RawCapture) {
    const int y = TH_BODY + 8;
    frame.fillRoundRect(12, y - s, 216, 46, 6, C_INPUT);
    frame.setTextDatum(ML_DATUM);
    frame.setTextColor(C_TEXT, C_INPUT);
    frame.drawString("FRAMES " + String(rawCaptureFrames()), 20, y - s + 14, 2);
    frame.setTextColor(C_MUTED, C_INPUT);
    frame.drawString(rawCaptureBreakdown(), 20, y - s + 33, 1);
    frame.setTextDatum(TL_DATUM);

    const int stopY = y + 46 + 8;
    frame.fillRoundRect(12, stopY - s, 216, 40, 8, C_DANGER);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(C_BG, C_DANGER);
    frame.drawString("STOP", 120, stopY - s + 20, 2);
    frame.setTextDatum(TL_DATUM);
    return;
  }

  static const char* const MODES[2] = {"PCAP FILE", "RAW COUNT"};
  for (int i = 0; i < 2; ++i) {
    const int bx = 12 + i * 110;
    const bool selm = (captureRaw == (i == 1));
    frame.fillRoundRect(bx, CAP_MODE_Y - s, 106, 26, 5, selm ? C_ACCENT : C_CARD_2);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(selm ? C_BG : C_TEXT, selm ? C_ACCENT : C_CARD_2);
    frame.drawString(MODES[i], bx + 53, CAP_MODE_Y - s + 13, 2);
    frame.setTextDatum(TL_DATUM);
  }

  if (captureRaw) {
    for (int i = 0; i < 5; ++i) {
      const int bx = 12 + i * 43;
      const bool sel = (static_cast<int>(rawFilter) == i);
      frame.fillRoundRect(bx, CAP_FILTER_Y - s, 41, 26, 5, sel ? C_ACCENT : C_CARD_2);
      frame.setTextDatum(MC_DATUM);
      frame.setTextColor(sel ? C_BG : C_TEXT, sel ? C_ACCENT : C_CARD_2);
      frame.drawString(RAW_FILTER_LABELS[i], bx + 20, CAP_FILTER_Y - s + 13, 1);
      frame.setTextDatum(TL_DATUM);
    }
  }

  drawChannelGrid(CAP_GRID_Y - s);

  frame.fillRoundRect(12, CAP_START_Y - s, 216, 40, 8, C_ACCENT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString(String(captureRaw ? "START RAW  CH " : "START PCAP  CH ") + String(selectedChannel),
                   120, CAP_START_Y - s + 20, 2);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::handleCapturePageTap(int16_t x, int16_t contentY) {
  if (toolHeaderBackHit(contentY)) { attackScreen = AttackScreen::Menu; redraw = true; return; }

  const AttackMode cur = marauderCurrentAttack();
  if (cur == AttackMode::PcapCapture) {
    const int stopY = statsStopY();
    if (contentY >= stopY && contentY < stopY + 40) { marauderStopAttack(); redraw = true; }
    return;
  }
  if (cur == AttackMode::RawCapture) {
    const int stopY = TH_BODY + 8 + 46 + 8;
    if (contentY >= stopY && contentY < stopY + 40) { rawCaptureStop(); redraw = true; }
    return;
  }

  if (contentY >= CAP_MODE_Y && contentY < CAP_MODE_Y + 26) {
    captureRaw = (x >= 122);
    redraw = true;
    return;
  }
  if (captureRaw && contentY >= CAP_FILTER_Y && contentY < CAP_FILTER_Y + 26) {
    const int i = (x - 12) / 43;
    if (i >= 0 && i <= 4) { rawFilter = static_cast<RawCaptureFilter>(i); redraw = true; }
    return;
  }

  const int ch = channelGridHit(x, contentY, CAP_GRID_Y);
  if (ch) { selectedChannel = (uint8_t)ch; redraw = true; return; }

  if (contentY >= CAP_START_Y && contentY < CAP_START_Y + 40) {
    if (captureRaw) rawCaptureStart(rawFilter, selectedChannel);
    else            marauderStartAttack(AttackMode::PcapCapture, selectedChannel);
    redraw = true;
  }
}

// ── Passive deauth/disassoc monitor page ─────────────────────────────────────
void ScreenUI::drawDeauthDetPage() {
  drawToolHeader("DEAUTH DETECT");
  const int s = activeScroll();
  const AttackMode cur = marauderCurrentAttack();

  if (cur == AttackMode::DeauthDetect) {
    const int y = TH_BODY + 8;
    const bool alert = deauthDetectorAlert();
    const uint16_t boxBg = alert ? C_DANGER : C_INPUT;
    const uint16_t fg    = alert ? C_BG     : C_TEXT;

    frame.fillRoundRect(12, y - s, 216, 62, 6, boxBg);
    frame.setTextDatum(ML_DATUM);
    frame.setTextColor(fg, boxBg);
    frame.drawString("FRAMES " + String(deauthDetectorCount()), 20, y - s + 16, 2);
    frame.setTextColor(alert ? C_BG : C_MUTED, boxBg);
    frame.drawString(deauthDetectorStatusLine(), 20, y - s + 42, 2);
    if (alert) {
      frame.setTextDatum(MR_DATUM);
      frame.setTextColor(C_BG, boxBg);
      frame.drawString("ALERT", 220, y - s + 16, 2);
    }
    frame.setTextDatum(TL_DATUM);

    const int stopY = y + 62 + 8;
    frame.fillRoundRect(12, stopY - s, 216, 40, 8, C_DANGER);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(C_BG, C_DANGER);
    frame.drawString("STOP", 120, stopY - s + 20, 2);
    frame.setTextDatum(TL_DATUM);
    return;
  }

  frame.setTextColor(C_MUTED, C_BG);
  frame.setCursor(14, TH_BODY + 6 - s);
  frame.print("Passive monitor for deauth and");
  frame.setCursor(14, TH_BODY + 24 - s);
  frame.print("disassoc frames. Flags attacks");

  drawChannelGrid(TH_BODY + 60 - s);

  const int startY = TH_BODY + 60 + 2 * 28 + 10;
  frame.fillRoundRect(12, startY - s, 216, 40, 8, C_ACCENT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString("START  CH " + String(selectedChannel), 120, startY - s + 20, 2);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::handleDeauthDetPageTap(int16_t x, int16_t contentY) {
  if (toolHeaderBackHit(contentY)) { attackScreen = AttackScreen::Menu; redraw = true; return; }

  if (marauderCurrentAttack() == AttackMode::DeauthDetect) {
    const int stopY = TH_BODY + 8 + 62 + 8;
    if (contentY >= stopY && contentY < stopY + 40) { marauderStopAttack(); redraw = true; }
    return;
  }

  const int ch = channelGridHit(x, contentY, TH_BODY + 60);
  if (ch) { selectedChannel = (uint8_t)ch; redraw = true; return; }

  const int startY = TH_BODY + 60 + 2 * 28 + 10;
  if (contentY >= startY && contentY < startY + 40) {
    marauderStartAttack(AttackMode::DeauthDetect, selectedChannel);
    redraw = true;
  }
}

// ── PMKID CAPTURE page ───────────────────────────────────────────────────────
//
// When idle: a scrollable list of APs from the last scan (shared with the
// deauth picker). Tap a row to select it as the PMKID target, then START.
// When running: live attempts / captures count, plus the auto-stop note.
void ScreenUI::drawPmkidPage() {
  drawToolHeader("PMKID CAPTURE");
  const int s = activeScroll();

  if (pmkidActive()) {
    const int y = TH_BODY + 8;

    // Stats box.
    frame.fillRoundRect(12, y - s, 216, 62, 6, C_INPUT);
    frame.setTextDatum(ML_DATUM);
    frame.setTextColor(C_TEXT, C_INPUT);
    frame.drawString("ATTEMPTS " + String(pmkidAttempts()), 20, y - s + 16, 2);
    frame.setTextColor(pmkidCaptures() ? C_ACCENT : C_MUTED, C_INPUT);
    frame.drawString("CAPTURES " + String(pmkidCaptures()), 20, y - s + 42, 2);
    frame.setTextDatum(TL_DATUM);

    // Auto-stop hint.
    frame.setTextColor(C_MUTED, C_BG);
    frame.setCursor(14, y - s + 72);
    frame.print("Auto-stops after 60 s of no hits.");

    // STOP.
    const int stopY = y + 62 + 32;
    frame.fillRoundRect(12, stopY - s, 216, 40, 8, C_DANGER);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(C_BG, C_DANGER);
    frame.drawString("STOP", 120, stopY - s + 20, 2);
    frame.setTextDatum(TL_DATUM);
    return;
  }

  // Idle: AP list from the shared scan cache.
  const int LIST_Y = TH_BODY;
  const int VISIBLE = 5;
  const int ROW_H = 32;

  if (marauderScanResultCount() == 0) {
    frame.setTextColor(C_MUTED, C_BG);
    frame.setCursor(14, LIST_Y - s + 8);
    frame.print("No APs. Tap RESCAN below.");
  } else {
    for (int r = 0; r < VISIBLE; ++r) {
      const size_t idx = pmkidListOffset + r;
      String ssid, bssid;
      int ch = 0, rssi = 0;
      if (!marauderScanResultGet(idx, ssid, bssid, ch, rssi)) continue;
      const int rowY = LIST_Y - s + r * ROW_H;
      frame.fillRoundRect(12, rowY, 216, ROW_H - 4, 5, C_CARD_2);
      frame.setTextColor(C_TEXT, C_CARD_2);
      frame.setCursor(18, rowY + 5);
      frame.print(shortText(ssid.length() ? ssid : String("<hidden>"), 24));
      frame.setTextColor(C_MUTED, C_CARD_2);
      frame.setCursor(18, rowY + 18);
      frame.print(shortText(bssid, 17));
      frame.setCursor(170, rowY + 18);
      frame.printf("ch%d %d", ch, rssi);
    }
  }

  const int btnY = LIST_Y - s + VISIBLE * ROW_H + 4;

  // UP / DOWN / RESCAN
  frame.fillRoundRect(12, btnY, 68, 28, 6, C_CARD_2);
  frame.fillRoundRect(86, btnY, 68, 28, 6, C_CARD_2);
  frame.fillRoundRect(160, btnY, 68, 28, 6, C_CARD_2);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_TEXT, C_CARD_2);
  frame.drawString("UP",    46, btnY + 14, 2);
  frame.drawString("DOWN", 120, btnY + 14, 2);
  frame.drawString("RESCAN", 194, btnY + 14, 1);
  frame.setTextDatum(TL_DATUM);

  // START hint (the tap handler uses the tapped row, not a selection state).
  frame.setTextColor(C_MUTED, C_BG);
  frame.setCursor(14, btnY + 38);
  frame.print("Tap an AP to capture its PMKID.");
}

void ScreenUI::handlePmkidPageTap(int16_t x, int16_t contentY) {
  if (toolHeaderBackHit(contentY)) { attackScreen = AttackScreen::Menu; redraw = true; return; }

  if (pmkidActive()) {
    // STOP button position must match drawPmkidPage()'s running view.
    const int stopY = TH_BODY + 8 + 62 + 32;
    if (contentY >= stopY && contentY < stopY + 40) { marauderStopAttack(); redraw = true; }
    return;
  }

  const int LIST_Y = TH_BODY;
  const int VISIBLE = 5;
  const int ROW_H = 32;

  // Tap an AP row → select it and start PMKID capture on it.
  if (contentY >= LIST_Y && contentY < LIST_Y + VISIBLE * ROW_H) {
    const size_t idx = pmkidListOffset + (contentY - LIST_Y) / ROW_H;
    String ssid, bssid;
    int ch = 0, rssi = 0;
    if (marauderScanResultGet(idx, ssid, bssid, ch, rssi)) {
      if (ssid.length() == 0) {
        // Hidden SSID can't be attacked — the association request needs an SSID.
        redraw = true;
        return;
      }
      marauderSetPmkidTarget(bssid, ssid, (uint8_t)ch);
      marauderStartAttack(AttackMode::Pmkid, (uint8_t)ch);
      redraw = true;
    }
    return;
  }

  const int btnY = LIST_Y + VISIBLE * ROW_H + 4;

  // UP / DOWN / RESCAN.
  if (contentY >= btnY && contentY < btnY + 28) {
    if (x < 80) {
      if (pmkidListOffset >= VISIBLE) pmkidListOffset -= VISIBLE;
    } else if (x < 154) {
      if (pmkidListOffset + VISIBLE < marauderScanResultCount()) pmkidListOffset += VISIBLE;
    } else {
      marauderRefreshScanResults();
      pmkidListOffset = 0;
    }
    redraw = true;
  }
}

// ── WiFi Analyzer ────────────────────────────────────────────────────────────
void ScreenUI::drawWifiAnalyzer(int y) {
  const int s = activeScroll();
  drawCard(6, y - s, 228, 216, "WIFI ANALYZER");

  uint32_t peak = 1;
  for (int ch = 1; ch <= 13; ++ch) peak = max(peak, screenTelemetry.channelHits[ch]);

  const int curCh = marauderAttackActive()
                        ? marauderCurrentChannel()
                        : (WiFi.channel() ? WiFi.channel() : screenTelemetry.channel);

  for (int ch = 1; ch <= 13; ++ch) {
    const int rowY = y - s + 18 + (ch - 1) * 15;
    const int barW = static_cast<int>(151UL * screenTelemetry.channelHits[ch] / peak);
    const bool cur = (ch == curCh);
    frame.setTextColor(cur ? C_ACCENT : C_MUTED, C_CARD);
    frame.setCursor(13, rowY + 2);
    frame.printf("%2d", ch);
    frame.fillRoundRect(35, rowY, 157, 10, 3, C_INPUT);
    if (barW > 0) frame.fillRoundRect(35, rowY, barW, 10, 3, cur ? C_WARN : C_ACCENT);
    frame.setTextColor(C_TEXT, C_CARD);
    frame.setCursor(199, rowY + 2);
    frame.print(screenTelemetry.channelHits[ch]);
  }
}

void ScreenUI::drawWifiAnalyzerPage() {
  drawToolHeader("WIFI ANALYZER");
  drawWifiAnalyzer(TH_Y + TH_H + 4);
}

void ScreenUI::handleWifiAnalyzerTap(int16_t contentY) {
  if (toolHeaderBackHit(contentY)) { attackScreen = AttackScreen::Menu; redraw = true; }
}

// ── WPS vulnerability check ──────────────────────────────────────────────────
namespace {
constexpr int WPS_SCAN_Y = TH_BODY;
constexpr int WPS_LIST_Y = TH_BODY + 36 + 18;
constexpr int WPS_VISIBLE = 4;
constexpr int WPS_ROW_H   = 30;
}

void ScreenUI::drawWpsPage() {
  drawToolHeader("WPS CHECK");
  const int s = activeScroll();

  frame.fillRoundRect(12, WPS_SCAN_Y - s, 150, 30, 6, C_ACCENT);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_BG, C_ACCENT);
  frame.drawString("SCAN", 87, WPS_SCAN_Y - s + 15, 2);
  const bool haveResults = wpsApCount() > 0;
  frame.fillRoundRect(168, WPS_SCAN_Y - s, 60, 30, 6, haveResults ? C_CARD_2 : C_INPUT);
  frame.setTextColor(haveResults ? C_TEXT : C_MUTED, haveResults ? C_CARD_2 : C_INPUT);
  frame.drawString("LOG", 198, WPS_SCAN_Y - s + 15, 2);
  frame.setTextDatum(TL_DATUM);

  frame.setTextColor(C_MUTED, C_BG);
  frame.setCursor(14, WPS_SCAN_Y + 36 - s);
  frame.printf("%u APs   %u WPS-enabled",
               (unsigned)wpsApCount(), (unsigned)wpsEnabledCount());

  const int listY = WPS_LIST_Y - s;
  if (!haveResults) {
    frame.setTextColor(C_MUTED, C_BG);
    frame.setCursor(14, listY + 8);
    frame.print("Tap SCAN to check nearby APs.");
    return;
  }

  for (int r = 0; r < WPS_VISIBLE; ++r) {
    const size_t idx = wpsOffset + r;
    String ssid, bssid; int ch = 0, rssi = 0; bool wps = false;
    if (!wpsGet(idx, ssid, bssid, ch, rssi, wps)) continue;
    const int rowY = listY + r * WPS_ROW_H;
    frame.fillRoundRect(12, rowY, 216, WPS_ROW_H - 4, 5, wps ? C_DANGER : C_CARD_2);
    frame.setTextColor(wps ? C_BG : C_TEXT, wps ? C_DANGER : C_CARD_2);
    frame.setCursor(18, rowY + 4);
    frame.print(shortText(ssid.length() ? ssid : String("<hidden>"), 22));
    frame.setTextColor(wps ? C_BG : C_MUTED, wps ? C_DANGER : C_CARD_2);
    frame.setCursor(18, rowY + 16);
    frame.printf("ch%d %ddBm  %s", ch, rssi, wps ? "WPS" : "-");
  }

  const int btnY = listY + WPS_VISIBLE * WPS_ROW_H;
  frame.fillRoundRect(12, btnY, 104, 28, 6, C_CARD_2);
  frame.fillRoundRect(124, btnY, 104, 28, 6, C_CARD_2);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_TEXT, C_CARD_2);
  frame.drawString("UP", 64, btnY + 14, 2);
  frame.drawString("DOWN", 176, btnY + 14, 2);
  frame.setTextDatum(TL_DATUM);
}

void ScreenUI::handleWpsPageTap(int16_t x, int16_t contentY) {
  if (toolHeaderBackHit(contentY)) { attackScreen = AttackScreen::Menu; redraw = true; return; }

  if (contentY >= WPS_SCAN_Y && contentY < WPS_SCAN_Y + 30) {
    if (x < 168) { wpsScanRun(); wpsOffset = 0; wpsLogResults(); }
    else if (wpsApCount()) { wpsLogResults(); }
    redraw = true;
    return;
  }

  if (wpsApCount() == 0) return;

  const int btnY = WPS_LIST_Y + WPS_VISIBLE * WPS_ROW_H;
  if (contentY >= btnY && contentY < btnY + 28) {
    if (x < 120) {
      if (wpsOffset >= WPS_VISIBLE) wpsOffset -= WPS_VISIBLE;
    } else {
      if (wpsOffset + WPS_VISIBLE < wpsApCount()) wpsOffset += WPS_VISIBLE;
    }
    redraw = true;
  }
}