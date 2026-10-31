#pragma once

#include <Arduino.h>
#include "Gfx.h"
#include "MarauderTools.h"
#include "BleSpam.h"
#include "RawCapture.h"
#include "PmkidCapture.h"
#include "Touch.h"

class ScreenUI {
public:
  void begin();
  void update(float speedValue);
  void handleTouch();
  void requestRedraw();
  void showSplash();
  void triggerHomeUpload();  // exposed for serial-command testing
  void triggerCoreMode();    // exposed for serial-command testing
  void triggerRadioIdle();   // OFF mode: tear down all radio consumers

private:
  enum class Page : uint8_t {
    Home,
    Nodes,
    Files,
    System,
    Marauder,
    Tools
  };
  static constexpr int PAGE_N = 6;

  enum class CsvColumns : uint8_t {
    Network,
    Security,
    Position
  };

  void draw();
  void drawHeader();
  void drawFooter();
  void drawShibaLogo(int x, int y);
  void drawPigFace(int cx, int cy, int radius);  // scalable pig for the splash
  void drawStatusChip(int x, const char* label, bool ok, bool warn = false);
  void drawCard(int x, int y, int w, int h, const char* title);
  void drawMetric(int x, int y, int w, const char* label, const String& value, uint16_t color);
  void drawScrollbar(int contentHeight);
  void drawSidePanel();   // W550 landscape live panel right of the page column (no-op on CYD)

  void drawHome();
  void drawNodes();
  void drawNodeDetail();        // per-node sub-cluster + channel editor (Core mode)
  void drawProfilesView();      // saved wardriving profiles list (Core mode)
  void drawGpsSection(int y);   // GPS info block, appended to the Home page
  void drawFiles();
  void drawCsvPreview();
  void drawSystem();
  int  drawSystemTools(int y);   // diagnostics/telemetry block atop the SYS page
  void drawMarauder();
  void handleMarauderTap(int16_t x, int16_t contentY);
  int  drawStats(int y);        // stats block, embedded in the Tools page; returns next y
  void drawSpeedo();
  void drawEditor();

  void openEditor(int settingIndex);
  void openProfileNameEditor(int slot);  // name a profile before saving it
  void handleEditorTap(int16_t x, int16_t y);
  void persistConfig();

  void handleTap(int16_t x, int16_t y);
  void handleHomeTap(int16_t x, int16_t contentY);
  void handleNodesTap(int16_t x, int16_t contentY);
  void handleNodeDetailTap(int16_t x, int16_t contentY);
  void handleProfilesTap(int16_t x, int16_t contentY);
  void handleFilesTap(int16_t x, int16_t contentY);
  void handleSystemTap(int16_t x, int16_t contentY);

  // Marauder is a menu of tools; each tool has its own page (config when idle,
  // live stats + STOP when running).
  enum class AttackScreen : uint8_t {
    Menu,
    Beacon,
    Deauth,
    Sniff,
    Capture,   // combined PCAP-to-SD + raw subtype counting
    DeauthDet, // passive deauth/disassoc monitor
    Analyzer,
    Wps,
    Ble,       // BLE spam variants (Apple / SwiftPair / Samsung / Fast Pair)
    Pmkid      // clientless PMKID capture
  };

  void drawToolsMarauder();            // the tool menu card on the Tools page
  void drawBeaconPage();
  void drawDeauthPage();
  void drawChannelPage(const char* title, AttackMode mode);  // Sniff / Pcap
  void drawBlePage();                  // BLE spam picker + running view
  void drawCapturePage();              // combined PCAP + raw subtype capture
  void drawDeauthDetPage();            // passive deauth/disassoc monitor
  void drawWifiAnalyzer(int y);        // channel occupancy bar chart (reusable)
  void drawWifiAnalyzerPage();
  void drawWpsPage();                  // WPS vulnerability check
  void drawPmkidPage();                // clientless PMKID capture
  void handleBeaconPageTap(int16_t x, int16_t contentY);
  void handleDeauthPageTap(int16_t x, int16_t contentY);
  void handleChannelPageTap(int16_t x, int16_t contentY, AttackMode mode);
  void handleBlePageTap(int16_t x, int16_t contentY);
  void handleCapturePageTap(int16_t x, int16_t contentY);
  void handleDeauthDetPageTap(int16_t x, int16_t contentY);
  void handleWifiAnalyzerTap(int16_t contentY);
  void handleWpsPageTap(int16_t x, int16_t contentY);
  void handlePmkidPageTap(int16_t x, int16_t contentY);

  // Shared tool-page furniture.
  int  drawToolHeader(const char* title);   // back bar; returns content-Y below it
  int  drawRunningStats(int y);             // TX/RX stats box; returns STOP button Y
  int  statsStopY() const;                  // content-Y of the STOP button (tap side)
  bool toolHeaderBackHit(int16_t contentY) const;
  // Compact channel grid: squares 1..14; hit-test returns 1..14 or 0. gridTop is content-Y.
  void drawChannelGrid(int gridTop);
  int  channelGridHit(int16_t x, int16_t contentY, int gridTop) const;

  AttackScreen attackScreen = AttackScreen::Menu;
  uint8_t      selectedChannel = 6;
  size_t       bssidPickerOffset = 0;
  BeaconSource beaconSrc = BeaconSource::RickRoll;
  bool         beaconHop = false;
  RawCaptureFilter rawFilter = RawCaptureFilter::AllMgmt;
  bool         captureRaw = false;   // CAPTURE tool: false = PCAP file, true = raw count
  size_t       wpsOffset = 0;

  // Multi-target deauth picker scroll position.
  size_t       deauthListOffset = 0;

  // BLE spam picker: which variant is currently selected.
  BleSpamType  bleType = BleSpamType::Apple;

  // PMKID capture picker scroll position (shares the scan-result cache with
  // the deauth picker — same source list).
  size_t       pmkidListOffset = 0;

  void setPage(Page next);
  void setWardMode();
  void setWebMode();
  bool webModeActive() const;

  int contentHeightForPage() const;
  int& activeScroll();
  int maxScroll() const;
  void setScroll(int value);

  size_t countCsvFiles() const;
  bool csvFileAt(size_t index, String& path, String& name, uint32_t& size) const;
  bool readCsvRecord(const String& path, size_t row, String& ssid, String& bssid,
                     String& auth, String& firstSeen, int& channel, int& rssi,
                     double& lat, double& lon) const;

  void enterSoloMode();
  void enterCoreUiMode();
  void enterNodeUiMode();
  void enterRadioIdleMode();  // OFF: stop scan/BLE/mesh, park Wi-Fi idle

  void uploadLogsViaHome();  // FILE screen: connect to home Wi-Fi + upload all CSVs

  GfxDevice tft;
  GfxSprite frame{&tft};
  FT6336Touch touch;

  void present();  // blit the 240x320 frame to the physical panel

  Page page = Page::Home;
  int scroll[PAGE_N] = {};
  bool redraw = true;
  bool spriteReady = false;
  uint32_t lastDrawMs = 0;
  float displayedSpeed = 0.0f;

  bool touchActive = false;
  int16_t touchStartX = 0;
  int16_t touchStartY = 0;
  int16_t touchLastX = 0;
  int16_t touchLastY = 0;
  uint32_t touchStartMs = 0;

  size_t filesOffset = 0;
  int selectedFile = -1;
  bool csvPreview = false;
  size_t csvRowOffset = 0;
  CsvColumns csvColumns = CsvColumns::Network;

  // SYS page on-screen editor for text / numeric settings.
  bool speedoMode = false;  // TOOL: full-screen speed/heading/collection HUD

  bool editing = false;
  int editSetting = -1;
  String editBuffer;
  uint8_t editLayer = 0;  // 0 = lower, 1 = upper, 2 = symbols
  bool lastSaveOk = true;

  // On-screen keyboard target: a config setting, or a wardriving profile name.
  enum class EditTarget : uint8_t { Setting, ProfileName };
  EditTarget editTarget = EditTarget::Setting;
  int editProfileSlot = -1;

  // NODES page sub-views (Core mode). Mutually exclusive with the list.
  bool nodeDetail = false;    // per-node sub-cluster/channel editor
  int  selectedNode = -1;     // index into coreNodes[] being edited
  bool profilesView = false;  // saved-profiles list
  int  profileSaveTarget = -1; // slot awaiting a "save current" confirmation (-1 = none)
};

extern ScreenUI screenUI;