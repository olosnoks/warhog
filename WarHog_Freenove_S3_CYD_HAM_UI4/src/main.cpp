#include <Arduino.h>
#include <esp_arduino_version.h>

#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "WarHog v2.59 requires Arduino-ESP32 core 3.x or newer. Use the pinned pioarduino platform in platformio.ini."
#endif
#include <sys/time.h>
#include "MarauderTools.h"
#include "BleSpam.h"
#include "AirTagMonitor.h"
#include "FlipperSniff.h"
#include "PwnagotchiDetect.h"
#include "RawCapture.h"
#include "DeauthDetector.h"
#include "HashcatWriter.h"
#include "PmkidCapture.h"
#include "BoardPins.h"
#include "StorageCompat.h"
#include "Globals.h"
#include "Config.h"
#include "GPS.h"
#include "SDUtils.h"
#include "SerialTool.h"
#include "Display.h"
#include "WiFiManager.h"
#include "Scanner.h"
#include "WigleUpload.h"
#include "WebUI.h"
#include "MeshNode.h"
#include "battery_test.h"
#include "BatteryMonitor.h"
#include "BleScanner.h"
#include "SessionStats.h"

#if defined(BOARD_W550)
#include <SPI.h>
SPIClass sdSpi(HSPI);   // non-static: sdTryRecover() (SDUtils.cpp) remounts on this instance

static bool initSd() {
  // Guition JC8048W550: onboard TF slot wired for SPI on GPIO 10/11/12/13.
  sdSpi.begin(BoardPins::SD_SCK, BoardPins::SD_MISO, BoardPins::SD_MOSI, BoardPins::SD_CS);

  // Mount at the fastest stable clock; a later wedge is cleared by sdTryRecover().
  if (!sdBeginBestClock(BoardPins::SD_CS)) {
    Serial.println("[SD] SD (SPI) mount failed");
    return false;
  }

  if (SD.cardType() == CARD_NONE) {
    Serial.println("[SD] No card detected");
    return false;
  }

  Serial.printf(
      "[SD] %llu MB total, %llu MB used\n",
      SD.totalBytes() / (1024ULL * 1024ULL),
      SD.usedBytes() / (1024ULL * 1024ULL));

  return true;
}
#else
static bool initSd() {
  // Mount at the best SDIO clock (setPins is done inside); a later wedge is
  // cleared by sdTryRecover(), which remounts one rung slower.
  if (!sdBeginBestClock(0)) {
    Serial.println("[SD] SD_MMC mount failed");
    return false;
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("[SD] No card detected");
    return false;
  }

  Serial.printf(
      "[SD] %llu MB total, %llu MB used\n",
      SD_MMC.totalBytes() / (1024ULL * 1024ULL),
      SD_MMC.usedBytes() / (1024ULL * 1024ULL));

  return true;
}
#endif

static void updateGpsState() {
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  const bool previousFix = gpsHasFix;
  gpsHasFix = gps.location.isValid() && gps.location.age() < 2000;

  if (gpsHasFix != previousFix) {
    Serial.printf("[GPS] %s\n", gpsHasFix ? "LOCKED" : "NO FIX");
  }

  if (gpsHasFix) {
    const float hdop = gps.hdop.isValid() ? gps.hdop.hdop() : 99.0f;
    const int sats = gps.satellites.isValid() ? static_cast<int>(gps.satellites.value()) : 0;

    if (hdop <= 10.0f && sats >= 3) {
      lastLat = gps.location.lat();
      lastLon = gps.location.lng();
      lastAlt = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
      lastAcc = hdop;
      lastGpsValid = true;
      lastGpsValidMs = millis();
    }
  }

  if (gpsHasFix &&
      gps.course.isValid() &&
      gps.course.age() < 2000 &&
      gps.speed.isValid() &&
      gps.speed.kmph() >= HEADING_MIN_SPEED_KMPH) {
    headingFeed(gps.course.deg());
  }

  static bool timeSet = false;
  if (!timeSet &&
      gps.date.isValid() &&
      gps.time.isValid() &&
      gps.date.age() < 5000 &&
      gps.time.age() < 5000) {
    struct tm t = {};
    t.tm_year = gps.date.year() - 1900;
    t.tm_mon = gps.date.month() - 1;
    t.tm_mday = gps.date.day();
    t.tm_hour = gps.time.hour();
    t.tm_min = gps.time.minute();
    t.tm_sec = gps.time.second();

    const time_t epoch = makeUtcEpochFromTm(&t);
    struct timeval now = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&now, nullptr);
    timeSet = true;
    Serial.println("[TIME] System time set from GPS");
  }
}

static void setupNetworkingAndUploads() {
  WiFi.mode(WIFI_STA);

  bool staOk = false;
  String meshMode = cfg.meshModeOnBoot;
  meshMode.toLowerCase();

  const bool coreBoot = meshMode == "core";
  const bool nodeBoot = meshMode == "node";
  const bool offBoot  = meshMode == "off";

  if (offBoot) {
    Serial.println("[BOOT] OFF (radio free) boot; skipping STA/AP");
  } else if (nodeBoot) {
    Serial.println("[BOOT] Mesh Node boot; skipping STA/AP");
  } else if (coreBoot) {
    Serial.println("[BOOT] Mesh Core boot; skipping STA/AP (Core uses ESP-NOW ch 6)");
  } else {
    staOk = connectSTA(12000);

    if (!staOk) {
      WiFi.setAutoReconnect(false);
      WiFi.persistent(false);
      WiFi.disconnect(true, true);
      delay(100);

      if (!coreBoot) {
        startAP();
      }
    }
  }

  lastStaStatus = WiFi.status();

  if (cfg.autoStartAfterUpload && staOk && !coreBoot && !nodeBoot) {
    wdgwarsDrainPendingJobs(5000);
    WiFi.setAutoReconnect(false);
    WiFi.persistent(false);
    WiFi.disconnect(true, false);
    delay(100);
    WiFi.mode(WIFI_STA);
    scanningEnabled = true;
    staOk = false;
  }

  startWebServer();

  if (staOk) {
    Serial.print("[WEB] STA IP: ");
    Serial.println(WiFi.localIP());
  } else if (apWindowActive) {
    Serial.print("[WEB] AP IP: ");
    Serial.println(WiFi.softAPIP());
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("=== WarHog Freenove S3 CYD HAM UI4 ===");
  Serial.printf("[BUILD] %s  (stats page + BLE + biscuit + home-upload button)\n", FIRMWARE_VERSION);
  Serial.println("[CMD] Serial: 'v'=version  'u'=trigger home upload  'b'=biscuit state  'c'=toggle core  'o'=OFF/radio-free");

  pins = PINS_S3;
  networksFound2G = 0;
  networksFound5G = 0;

  sdOk = initSd();
  coreOtaPreload();   // stage the node-update image in PSRAM while there is still RAM for SD reads

  if (sdOk) {
    loadConfigFromSD();
  }

  displayBegin();

  GPSSerial.setRxBufferSize(4096);
  GPSSerial.begin(cfg.gpsBaud, SERIAL_8N1, pins.gps_rx, pins.gps_tx);
  Serial.printf("[GPS] RX=%d TX=%d baud=%lu\n",
                pins.gps_rx,
                pins.gps_tx,
                static_cast<unsigned long>(cfg.gpsBaud));

  setupNetworkingAndUploads();

  if (sdOk) {
    deleteEmptyCsvs();
    const bool logOk = openLogFile();
    Serial.printf("[SD] Log file: %s\n", logOk ? "OK" : "FAIL");
  }

  if (sdOk && cfg.batteryTest) {
    batteryTestInit();
  }

  bleScannerBegin();

  marauderToolsBegin();
  bleSpamBegin();
  airTagMonitorBegin();
  flipperSniffBegin();
  pwnagotchiDetectBegin();
  rawCaptureBegin();
  deauthDetectorBegin();
  hashcatWriterBegin();
  pmkidBegin();

  String meshMode = cfg.meshModeOnBoot;
  meshMode.toLowerCase();

  if (meshMode == "node") {
    currentPage = 5;
    enterNodeMode();
  } else if (meshMode == "core") {
    if (WiFi.status() == WL_CONNECTED || WiFi.getMode() != WIFI_OFF) {
      wdgwarsDrainPendingJobs(5000);
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      delay(150);
    }
    currentPage = 5;
    enterCoreMode();
  } else if (meshMode == "off") {
    displayEnterRadioIdle();
  }

  displayRequestRedraw();
  Serial.println("=== Boot complete ===");
}

// Handle a legacy single-character debug command (v/u/b/c/o).
static void handleCharCommand(char c) {
  {
    if (c == 'v' || c == 'V') {
      Serial.printf("[CMD] FW %s  heap=%u  battPin=%d  battAvail=%d  battV=%.2f  raw_mV=%d\n",
                    FIRMWARE_VERSION, ESP.getFreeHeap(), cfg.battPin,
                    (int)batteryMonitorAvailable(), batteryVoltage(),
                    (cfg.battPin >= 1 && cfg.battPin <= 20) ? analogReadMilliVolts(cfg.battPin) : -1);
    } else if (c == 'u' || c == 'U') {
      Serial.println("[CMD] Trigger home upload...");
      displayTriggerHomeUpload();
      Serial.printf("[CMD] Home upload returned. lastResult='%s' staIP=%s\n",
                    uploadLastResult.c_str(),
                    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "(not connected)");
    } else if (c == 'b' || c == 'B') {
      Serial.printf("[CMD] biscuit report cfg=%d active=%d linked=%d sent=%lu\n",
                    (int)cfg.biscuitReport, (int)biscuitReporterActive, (int)biscuitLinked,
                    (unsigned long)biscuitRecordsSent);
    } else if (c == 'c' || c == 'C') {
      Serial.printf("[CMD] toggle core (was core=%d node=%d) heap=%u\n",
                    (int)meshCoreActive, (int)meshNodeActive, ESP.getFreeHeap());
      displayToggleCore();
      Serial.printf("[CMD] toggle core done: core=%d heap=%u\n",
                    (int)meshCoreActive, ESP.getFreeHeap());
    } else if (c == 'o' || c == 'O') {
      Serial.printf("[CMD] OFF/radio-free (was idle=%d core=%d node=%d)\n",
                    (int)radioIdle, (int)meshCoreActive, (int)meshNodeActive);
      displayEnterRadioIdle();
      Serial.printf("[CMD] radio idle=%d wifiMode=%d heap=%u\n",
                    (int)radioIdle, (int)WiFi.getMode(), ESP.getFreeHeap());
    }
  }
}

// Read serial: a bare legacy command char (v/u/b/c/o) is handled immediately;
// everything else is accumulated into a line and, on newline, dispatched to the
// "WT" toolkit protocol (SerialTool). Non-WT lines are ignored there.
static void processSerialCommands() {
  static char     lineBuf[640];
  static uint16_t lineLen = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (lineLen == 0 && (c=='v'||c=='V'||c=='u'||c=='U'||c=='b'||c=='B'||
                         c=='c'||c=='C'||c=='o'||c=='O')) {
      handleCharCommand(c);
      continue;
    }
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) { lineBuf[lineLen] = 0; serialToolHandleLine(lineBuf); lineLen = 0; }
      continue;
    }
    if (lineLen < (uint16_t)(sizeof(lineBuf) - 1)) lineBuf[lineLen++] = c;
    else lineLen = 0;  // overflow — drop the line
  }
}

void loop() {
  server.handleClient();
  processSerialCommands();

  if (apWindowActive) {
    const wifi_mode_t mode = WiFi.getMode();
    if ((mode == WIFI_AP || mode == WIFI_AP_STA) &&
        WiFi.softAPgetStationNum() > 0) {
      apClientSeen = true;
    }
  }

  stopAPIfAllowed();
  updateGpsState();
  displayHandleTouch();

  const float speedKmph = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
  const float speedDisplay = cfg.speedUnits == "mph"
      ? speedKmph * 0.621371f
      : speedKmph;

  displayLoop(speedDisplay);

  if (!meshNodeActive && !meshCoreActive && !radioIdle) {
    handleStaTransitions();
    checkHomeNetworkReturn();
  }

  wdgwarsServicePendingJobs();

  marauderToolsTick();
  bleSpamTick();
  airTagMonitorTick();
  flipperSniffTick();
  pwnagotchiDetectTick();
  rawCaptureTick();

  // Solo wardriving streams to the phone app over BLE too (cfg.blePhoneSolo).
  // Outside the mesh pages a mesh role may still be live, so the tick itself
  // checks the role flags and tears the bridge down when one is.
  phoneBridgeSoloTick();

  if (currentPage == 5) {
    if (meshCoreActive) coreModeTick();
    else if (meshNodeActive) nodeModeTick();
  } else {
    autoPaused = shouldPauseScanning();
    const wifi_mode_t mode = WiFi.getMode();
    const bool apActive = mode == WIFI_AP || mode == WIFI_AP_STA;

    const bool allowScan = scanningEnabled &&
                           sdOk &&
                           !statusPagePaused &&
                           !apActive &&
                           !serialToolActive() &&  // a USB toolkit session owns the SD bus
                           !meshCoreActive &&   // Core only logs node records — never scans APs itself
                           !meshNodeActive &&   // Node scans via nodeModeTick(), not the solo scanner
                           (userScanOverride || !autoPaused);

    allowScanForOled = allowScan;

    if (allowScan && !marauderAttackActive()) {
      doScanOnce();
    }
  }

  sdLogBufferTick();   // time-based safety flush for the batched CSV writer
  serialToolTick();    // USB toolkit session inactivity timeout
  batteryTestTick();
  batteryMonitorTick();
  bleScannerTick();
  statsHistoryTick();
  statsRateTick();
  delay(10);
}
