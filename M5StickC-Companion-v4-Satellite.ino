/*
  ------------------------------------------------------------
  M5StickC Companion v4 Single-Button Satellite
  Author: Adrian Davis

  Hardware:
    - M5StickC (ESP32)
    - Built-in TFT (landscape mode: 160x80)

  Features:
    - Companion v4 Satellite API support over TCP (WiFiClient)
    - WiFiManager config portal (SSID = M5StickC_<MAC>)
    - Stores Companion IP and port in EEPROM
    - DeviceID = "M5StickC_" + full MAC (no colons, uppercase)
    - Text rendering on LCD:
        * Up to 3 lines static if they fit
        * Otherwise horizontal scrolling marquee
    - Uses:
        * TEXT (base64 decoded)
        * COLOR      = background colour
        * TEXTCOLOR  = text colour
        * BRIGHTNESS = 0–100 mapped to ScreenBreath
          - 0   -> ScreenBreath(0)
          - 1–100 -> ScreenBreath(7–50)

    - Front Button (BtnA):
        * Sends KEY-PRESS to Companion
          - PRESSED=true on press
          - PRESSED=false on release
        * During CONFIG? screen at boot, pressing BtnA
          immediately opens the config portal.

  Config behaviour:
    - WiFi fails => WiFiManager config portal (standard)
    - Companion down => keep retrying, NO config portal
    - Reset during 5s "CONFIG?" window still increments
      boot counter for legacy behaviour
  ------------------------------------------------------------
*/

#include <M5StickC.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <EEPROM.h>

// ------------------------ DISPLAY CONFIG ---------------------

const int SCREEN_W = 160;
const int SCREEN_H = 80;

// Text rendering / scroll
String currentText  = "";
String line1        = "";
String line2        = "";
String line3        = "";
int    numLines     = 0;   // 0..3
bool   scrolling    = false;

int    scrollX          = 0;
int    scrollWidth      = 0;
unsigned long lastScrollTime = 0;
const uint16_t scrollIntervalMs = 50;  // scrolling speed
int textSize = 2;            // reasonably chunky
const int lineHeight    = 16;          // approx for textSize=2

// Colours
uint16_t bgColor   = BLACK;
uint16_t txtColor  = WHITE;

// Brightness (0–100 from Companion)
int brightnessPercent = 20; // default

// ------------------------ COMPANION CONFIG ------------------

WiFiManager wifiManager;
WiFiClient  client;

// What we store in EEPROM
char companion_host[40] = "Companion IP";
char companion_port[6]  = "16622";

// WiFiManager custom params
WiFiManagerParameter* custom_companionIP;
WiFiManagerParameter* custom_companionPort;
WiFiManagerParameter* param_bootCount;   // info/debug param

// Device ID and hostname
String deviceID;

// AP password for config portal (blank = open)
const char* AP_password = "";

// EEPROM layout
// [0] = 'L', [1] = 'M' magic
// [2] = version
// [3..42]  = companion_host (40 bytes)
// [43..48] = companion_port (6 bytes)
// [60]     = bootCounter
const uint16_t EEPROM_SIZE      = 128;
const uint16_t EEPROM_BOOT_ADDR = 60;

// Timing / connection
unsigned long lastPingTime     = 0;
unsigned long lastConnectTry   = 0;
const unsigned long connectRetryMs  = 5000;
const unsigned long pingIntervalMs  = 2000;

// How many previous boots before forcing config portal
const uint8_t BOOT_FAIL_LIMIT = 1;

// Cached copy of PRE-INCREMENT boot counter (from EEPROM)
uint8_t bootCountCached = 0;

// Boot prompt timing
const unsigned long CONFIG_PROMPT_MS = 5000;

// ------------------------ BASE64 DECODER --------------------

const char* B64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Index(char c) {
  const char* p = strchr(B64_TABLE, c);
  if (!p) return -1;
  return (int)(p - B64_TABLE);
}

String decodeBase64(const String& input) {
  int len = input.length();
  int val = 0;
  int valb = -8;
  String out;

  for (int i = 0; i < len; i++) {
    char c = input[i];
    if (c == '=') break;
    int idx = b64Index(c);
    if (idx < 0) break;  // invalid char – stop decoding

    val = (val << 6) + idx;
    valb += 6;
    if (valb >= 0) {
      char outChar = (char)((val >> valb) & 0xFF);
      out += outChar;
      valb -= 8;
    }
  }

  return out;
}

// ------------------------ EEPROM Helpers -------------------

void eepromLoadCompanionConfig() {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(0) == 'L' && EEPROM.read(1) == 'M') {
    // Valid header
    for (int i = 0; i < 40; i++) {
      companion_host[i] = (char)EEPROM.read(3 + i);
    }
    companion_host[39] = '\0';

    for (int i = 0; i < 6; i++) {
      companion_port[i] = (char)EEPROM.read(43 + i);
    }
    companion_port[5] = '\0';
  }
  EEPROM.end();
}

void eepromSaveCompanionConfig(const char* host, const char* port) {
  EEPROM.begin(EEPROM_SIZE);

  EEPROM.write(0, 'L');
  EEPROM.write(1, 'M');
  EEPROM.write(2, 1); // version

  // host
  for (int i = 0; i < 40; i++) {
    char c = (i < (int)strlen(host)) ? host[i] : 0;
    EEPROM.write(3 + i, (uint8_t)c);
  }

  // port
  for (int i = 0; i < 6; i++) {
    char c = (i < (int)strlen(port)) ? port[i] : 0;
    EEPROM.write(43 + i, (uint8_t)c);
  }

  EEPROM.commit();
  EEPROM.end();
}

uint8_t eepromReadBootCounter() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t c = EEPROM.read(EEPROM_BOOT_ADDR);
  EEPROM.end();
  return c;
}

void eepromWriteBootCounter(uint8_t c) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_BOOT_ADDR, c);
  EEPROM.commit();
  EEPROM.end();
}

// ------------------------ WiFiManager helpers --------------

String getParam(const String& name) {
  if (wifiManager.server && wifiManager.server->hasArg(name)) {
    return wifiManager.server->arg(name);
  }
  return "";
}

// Apply Companion brightness (0–100) to M5StickC backlight
void applyBrightness() {
  int p = brightnessPercent;
  if (p < 0)   p = 0;
  if (p > 100) p = 100;

  uint8_t level;

  if (p == 0) {
    // Allow Companion 0 to be "as dark as possible"
    level = 0;
  } else {
    // Map 1–100 to a wider 7–50 range for more punch
    level = map(p, 1, 100, 7, 50);
    if (level < 7)  level = 7;
    if (level > 50) level = 50;
  }

  Serial.printf("[LCD] BRIGHTNESS percent=%d -> ScreenBreath=%u\n", p, level);
  M5.Axp.ScreenBreath(level);
}

void saveParamCallback() {
  String str_companionIP   = getParam("companionIP");
  String str_companionPort = getParam("companionPort");
  String str_bootCount     = getParam("bootCount");

  if (str_companionIP.length() > 0) {
    str_companionIP.toCharArray(companion_host, sizeof(companion_host));
  }
  if (str_companionPort.length() > 0) {
    str_companionPort.toCharArray(companion_port, sizeof(companion_port));
  }

  // Optional: allow user to override boot counter from portal
  if (str_bootCount.length() > 0) {
    uint8_t newBC = (uint8_t)str_bootCount.toInt();
    eepromWriteBootCounter(newBC);
    Serial.printf("[WiFi] Boot counter updated from portal: %u\n", newBC);
  }

  eepromSaveCompanionConfig(companion_host, companion_port);
}

// Forward declarations
void refreshDisplay();
void showConfigModeMessage();
void setTextNow(const String& txt);
void startConfigPortal();

// ------------------------ Text layout helpers --------------

// Word-wrap into up to 3 lines. Returns true if it fits.
bool wrapToLines(const String& src, String& l1, String& l2, String& l3, int& outLines) {
  l1 = "";
  l2 = "";
  l3 = "";
  outLines = 0;

  if (src.length() == 0) return true;

  M5.Lcd.setTextSize(textSize);

  // Split into words
  std::vector<String> words;
  int start = 0;
  while (start < (int)src.length()) {
    int space = src.indexOf(' ', start);
    if (space < 0) space = src.length();
    String w = src.substring(start, space);
    if (w.length() > 0) words.push_back(w);
    start = space + 1;
  }

  String* lines[3] = { &l1, &l2, &l3 };
  int currentLine = 0;

  for (size_t i = 0; i < words.size(); i++) {
    if (currentLine >= 3) {
      // would need more than 3 lines
      return false;
    }

    String candidate;
    if (lines[currentLine]->length() == 0) {
      candidate = words[i];
    } else {
      candidate = *lines[currentLine] + " " + words[i];
    }

    int w = M5.Lcd.textWidth(candidate);

    if (w <= SCREEN_W) {
      *lines[currentLine] = candidate;
    } else {
      // start next line with this word
      currentLine++;
      if (currentLine >= 3) {
        return false; // too many lines
      }
      *lines[currentLine] = words[i];
    }
  }

  outLines = currentLine + 1;
  return true;
}

void analyseLayout() {
  // Reset layout state
  line1 = "";
  line2 = "";
  line3 = "";
  numLines = 0;
  scrolling = false;

  int len = currentText.length();
  if (len == 0) return;

  // Default text size for normal / scrolling text
  textSize = 2;
  M5.Lcd.setTextSize(textSize);
  M5.Lcd.setTextWrap(false);

  // ---------- ULTRA BIG TEXT MODE (<= 3 chars, size 6) ----------
  if (len <= 3) {
    numLines = 1;
    line1    = currentText;
    textSize = 6;
    scrolling = false;      // never scroll in ultra-big mode
    return;
  }

  // ---------- BIG TEXT MODE (4–6 chars, size 4) ----------
  if (len <= 6) {
    numLines = 1;
    line1    = currentText;
    textSize = 4;
    scrolling = false;      // never scroll in big-text mode
    return;
  }

  // ---------- 3-LINE WRAP MODE ----------
  String w1, w2, w3;
  int lines = 0;
  bool fits = wrapToLines(currentText, w1, w2, w3, lines);

  if (fits && lines > 0 && lines <= 3) {
    // Text fits in up to 3 lines -> static layout
    numLines = lines;
    line1 = w1;
    line2 = (lines >= 2) ? w2 : "";
    line3 = (lines >= 3) ? w3 : "";
    scrolling = false;
    return;
  }

  // ---------- FALLBACK: SCROLLING MODE ----------
  scrolling = true;
  numLines  = 0;    // we ignore line1/2/3 in scrolling mode

  // Ensure width is based on correct text size (normal size)
  scrollWidth = M5.Lcd.textWidth(currentText);
  scrollX     = SCREEN_W;
  lastScrollTime = millis();
}

void refreshDisplay() {
  M5.Lcd.fillScreen(bgColor);
  M5.Lcd.setTextColor(txtColor, bgColor);
  M5.Lcd.setTextWrap(false);       // never wrap automatically
  M5.Lcd.setTextSize(textSize);

  if (currentText.length() == 0) return;

  // ---------- NON-SCROLLING MODES ----------
  if (!scrolling) {

    // ----- BIG / ULTRA-BIG TEXT MODE (<= 6 chars, size 4 or 6) -----
    if (currentText.length() <= 6) {
      M5.Lcd.setTextWrap(false);
      int w     = M5.Lcd.textWidth(currentText);
      int textH = 8 * textSize;          // approx glyph height
      int x     = (SCREEN_W - w) / 2;
      int y     = (SCREEN_H - textH) / 2;
      if (x < 0) x = 0;
      if (y < 0) y = 0;
      M5.Lcd.setCursor(x, y);
      M5.Lcd.print(currentText);
      return;
    }

    // ----- STATIC 1–3 LINE MODE -----
    if (numLines >= 1 && numLines <= 3) {
      String lines[3] = { line1, line2, line3 };

      int totalHeight = lineHeight * numLines;
      int startY = (SCREEN_H - totalHeight) / 2;
      int y = startY;

      for (int i = 0; i < numLines; i++) {
        int w = M5.Lcd.textWidth(lines[i]);
        int x = (SCREEN_W - w) / 2;
        if (x < 0) x = 0;
        M5.Lcd.setCursor(x, y);
        M5.Lcd.print(lines[i]);
        y += lineHeight;
      }
    }

  } else {
    // ---------- SCROLLING MODE ----------
    // Single-line marquee at the BOTTOM of the screen
    int h = lineHeight;
    int y = SCREEN_H - h;      // bottom line
    if (y < 0) y = 0;

    M5.Lcd.setTextWrap(false);
    M5.Lcd.setCursor(scrollX, y);
    M5.Lcd.print(currentText);
  }
}

void setTextNow(const String& txt) {
  currentText = txt;
  analyseLayout();
  refreshDisplay();
}

void setText(const String& txt) {
  setTextNow(txt);
}

// ------------------------ Colour parsing --------------------

// Parse token forms like:
//   COLOR=#RRGGBB
//   COLOR="##RRGGBB"
//   COLOR=R,G,B
// Returns true on success
bool parseColorToken(const String& line, const String& key, int &r, int &g, int &b) {
  int pos = line.indexOf(key);
  if (pos < 0) return false;

  pos += key.length();
  if (pos < (int)line.length() && line[pos] == '=') pos++;

  int end = line.indexOf(' ', pos);
  if (end < 0) end = line.length();

  String val = line.substring(pos, end);
  val.trim();
  if (val.length() == 0) return false;

  // Strip surrounding quotes if present, e.g. "#ff0000"
  if (val.length() >= 2 && val[0] == '\"' && val[val.length() - 1] == '\"') {
    val = val.substring(1, val.length() - 1);
  }

  // Hex form: #RRGGBB
  if (val[0] == '#') {
    if (val.length() < 7) return false;
    String rs = val.substring(1, 3);
    String gs = val.substring(3, 5);
    String bs = val.substring(5, 7);
    r = (int) strtol(rs.c_str(), nullptr, 16);
    g = (int) strtol(gs.c_str(), nullptr, 16);
    b = (int) strtol(bs.c_str(), nullptr, 16);
    return true;
  }

  // Decimal CSV form: R,G,B
  int c1 = val.indexOf(',');
  int c2 = val.indexOf(',', c1 + 1);
  if (c1 < 0 || c2 < 0) return false;

  r = val.substring(0, c1).toInt();
  g = val.substring(c1 + 1, c2).toInt();
  b = val.substring(c2 + 1).toInt();
  return true;
}

// Map Companion KEY-STATE colours to LCD
void handleKeyStateColor(const String& line) {
  int r, g, b;
  bool bgOk  = false;
  bool txtOk = false;

  if (parseColorToken(line, "COLOR", r, g, b)) {
    bgColor = M5.Lcd.color565(r, g, b);
    bgOk = true;
    Serial.printf("[API] BG COLOR parsed r=%d g=%d b=%d\n", r, g, b);
  }

  if (parseColorToken(line, "TEXTCOLOR", r, g, b)) {
    txtColor = M5.Lcd.color565(r, g, b);
    txtOk = true;
    Serial.printf("[API] TEXT COLOR parsed r=%d g=%d b=%d\n", r, g, b);
  }

  if (bgOk || txtOk) {
    Serial.printf("[API] COLORS bgOk=%s txtOk=%s\n",
                  bgOk ? "true" : "false",
                  txtOk ? "true" : "false");
    refreshDisplay();
  }
}

// ------------------------ Companion / API parsing ----------

void sendAddDevice() {
  String cmd = "ADD-DEVICE DEVICEID=" + deviceID +
               " PRODUCT_NAME=\"M5StickC\" KEYS_TOTAL=1 KEYS_PER_ROW=1 BITMAPS=0 COLORS=true TEXT=true";
  client.println(cmd);
  Serial.println("[API] Sent: " + cmd);
}

// Send KEY-PRESS when BtnA changes state
void sendKeyPress(bool pressed) {
  if (!client.connected()) return;
  String cmd = "KEY-PRESS DEVICEID=" + deviceID +
               " KEY=0 PRESSED=" + String(pressed ? "true" : "false");
  client.println(cmd);
  Serial.println("[API] Sent: " + cmd);
}

// Decode Companion TEXT (base64 → UTF-8 string)
String decodeCompanionText(const String& encoded) {
  if (encoded.length() == 0) return encoded;

  // Heuristic: if it contains only base64-ish chars, try decode
  bool looksBase64 = true;
  for (size_t i = 0; i < encoded.length(); i++) {
    char c = encoded[i];
    if (!((c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') ||
          c == '+' || c == '/' || c == '=')) {
      looksBase64 = false;
      break;
    }
  }

  if (!looksBase64) {
    return encoded;
  }

  String decoded = decodeBase64(encoded);
  if (decoded.length() == 0) {
    // if decode failed, fall back to original
    return encoded;
  }
  return decoded;
}

void handleKeyStateText(const String& line) {
  int tPos = line.indexOf("TEXT=");
  if (tPos < 0) {
    return; // no text in this update
  }

  // Find the first quote after TEXT=
  int firstQuote = line.indexOf('\"', tPos);
  if (firstQuote < 0) return;

  int secondQuote = line.indexOf('\"', firstQuote + 1);
  if (secondQuote < 0) return;

  String textField = line.substring(firstQuote + 1, secondQuote);

  // Companion is sending base64 for TEXT, e.g. "VEVTVA==" → "TEST"
  String decoded = decodeCompanionText(textField);

  // Handle escaped newlines in the decoded string
  decoded.replace("\\n", "\n");

  Serial.print("[API] TEXT encoded = \"");
  Serial.print(textField);
  Serial.print("\"  decoded = \"");
  Serial.print(decoded);
  Serial.println("\"");

  setText(decoded);
}

void parseAPI(const String& apiData) {
  if (apiData.length() == 0) return;

  if (apiData.startsWith("PONG")) {
    return;
  }

  Serial.println("[API] RX: " + apiData);

  if (apiData.startsWith("PING")) {
    String payload = apiData.substring(apiData.indexOf(' ') + 1);
    client.println("PONG " + payload);
    return;
  }

  if (apiData.startsWith("BRIGHTNESS")) {
    int valPos = apiData.indexOf("VALUE=");
    if (valPos >= 0) {
      String v = apiData.substring(valPos + 6);
      v.trim();
      brightnessPercent = v.toInt();
      Serial.println("[API] BRIGHTNESS set to " + String(brightnessPercent));
      applyBrightness();
    }
    return;
  }

  if (apiData.startsWith("KEYS-CLEAR")) {
    Serial.println("[API] KEYS-CLEAR");
    setText("");
    return;
  }

  if (apiData.startsWith("KEY-STATE")) {
    Serial.print("[API] KEY-STATE raw line = ");
    Serial.println(apiData);
    handleKeyStateColor(apiData);
    handleKeyStateText(apiData);
    return;
  }
}

// ------------------------ CONFIG PORTAL helpers ------------

void showConfigModeMessage() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(WHITE, BLACK);
  // simple two-line message
  M5.Lcd.setCursor(4, 20);
  M5.Lcd.print("CFG: 192.168.4.1");
  M5.Lcd.setCursor(4, 40);
  M5.Lcd.print(deviceID);
}

void startConfigPortal() {
  Serial.println("[WiFi] Entering CONFIG PORTAL mode");
  showConfigModeMessage();

  // No timeout when we explicitly call config mode
  wifiManager.setConfigPortalTimeout(0);

  // Start AP + portal, blocks until user saves or exits
  wifiManager.startConfigPortal(deviceID.c_str(), AP_password);

  // After returning, update our Companion host/port and persist
  strncpy(companion_host, custom_companionIP->getValue(), sizeof(companion_host));
  companion_host[sizeof(companion_host) - 1] = '\0';

  strncpy(companion_port, custom_companionPort->getValue(), sizeof(companion_port));
  companion_port[sizeof(companion_port) - 1] = '\0';

  eepromSaveCompanionConfig(companion_host, companion_port);

  // Reset boot counter so we do not immediately re-enter config
  eepromWriteBootCounter(0);

  // Show a small message so you know it applied
  setTextNow("CFG SAVED");
  delay(1000);
}

// ------------------------ WiFi / Initial Config ------------

void connectToNetwork() {
  WiFi.mode(WIFI_STA);

  // Load Companion config from EEPROM (for default field values)
  eepromLoadCompanionConfig();

  Serial.printf("[Boot] Cached boot counter (prev boot) = %u\n", bootCountCached);

  // ---------- Prepare WiFiManager with params BEFORE any portal ----------

  // Companion IP / Port params
  custom_companionIP   = new WiFiManagerParameter("companionIP", "Companion IP", companion_host, 40);
  custom_companionPort = new WiFiManagerParameter("companionPort", "Satellite Port", companion_port, 6);

  // Boot counter info param
  char bcStr[6];
  snprintf(bcStr, sizeof(bcStr), "%u", bootCountCached);
  param_bootCount = new WiFiManagerParameter("bootCount", "Boot Counter (info)", bcStr, 5);

  wifiManager.addParameter(custom_companionIP);
  wifiManager.addParameter(custom_companionPort);
  wifiManager.addParameter(param_bootCount);

  wifiManager.setSaveParamsCallback(saveParamCallback);

  std::vector<const char*> menu = { "wifi", "param", "info", "sep", "restart", "exit" };
  wifiManager.setMenu(menu);
  wifiManager.setClass("invert");
  wifiManager.setConfigPortalTimeout(180); // 3 minutes auto portal if WiFi fails

  wifiManager.setAPCallback([](WiFiManager* wm) {
    Serial.println("[WiFi] Config portal started");
    showConfigModeMessage();
  });

  // -----------------------------------------------------------------------
  // If previous boot requested config (via reset during CONFIG? window)
  // -----------------------------------------------------------------------
  if (bootCountCached >= BOOT_FAIL_LIMIT) {
    startConfigPortal();
    // startConfigPortal() resets boot counter to 0 on success
  }

  // Normal autoConnect behaviour (connect to WiFi, or start portal if no WiFi)
  bool res = wifiManager.autoConnect(deviceID.c_str(), AP_password);

  if (!res) {
    Serial.println("[WiFi] Failed to connect, restarting...");
    setTextNow("WiFi ERROR");
    delay(1000);
    ESP.restart();
  } else {
    Serial.print("[WiFi] Connected: ");
    Serial.println(WiFi.localIP());
    setTextNow("WiFi OK");
    delay(1000);
  }

  // Copy latest values
  strncpy(companion_host, custom_companionIP->getValue(), sizeof(companion_host));
  companion_host[sizeof(companion_host) - 1] = '\0';

  strncpy(companion_port, custom_companionPort->getValue(), sizeof(companion_port));
  companion_port[sizeof(companion_port) - 1] = '\0';

  eepromSaveCompanionConfig(companion_host, companion_port);

  // WiFi successfully connected => clear boot counter
  eepromWriteBootCounter(0);
  Serial.println("[Boot] Boot counter reset to 0 (WiFi OK)");
}

// ------------------------ SETUP ----------------------------

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[M5StickC] Booting...");

  // Landscape
  M5.Lcd.setRotation(1);

  // Build deviceID from MAC: M5StickC_<MAC>
  uint8_t mac[6];
  WiFi.macAddress(mac);

  char macBuf[13];
  sprintf(macBuf, "%02X%02X%02X%02X%02X%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  deviceID  = "M5StickC_";
  deviceID += macBuf;
  deviceID.toUpperCase();

  Serial.println("[ID] deviceID = " + deviceID);

  // Initial screen state
  bgColor  = BLACK;
  txtColor = WHITE;
  M5.Lcd.fillScreen(bgColor);
  M5.Lcd.setTextSize(textSize);

  // Boot counter logic
  bootCountCached = eepromReadBootCounter();
  uint8_t newCount = bootCountCached;
  if (newCount < 255) newCount++;
  eepromWriteBootCounter(newCount);
  Serial.printf("[Boot] Boot counter previous=%u, new=%u\n", bootCountCached, newCount);

  // Show "BOOT" for 1 second
  setTextNow("BOOT");
  delay(1000);

  // Show "CONFIG?" and wait for 5 seconds window
  // BtnA press here will immediately open config portal
  setTextNow("CONFIG?");
  unsigned long cfgPromptStart = millis();
  bool requestConfig = false;
  while (millis() - cfgPromptStart < CONFIG_PROMPT_MS) {
    M5.update();
    if (M5.BtnA.wasPressed()) {
      requestConfig = true;
      break;
    }
    delay(10);
  }

  if (requestConfig) {
    // Explicit request: zero out boot counter and go straight to config
    eepromWriteBootCounter(0);
    startConfigPortal();
  }

  // WiFi + config (with boot counter logic via bootCountCached)
  connectToNetwork();

  // After WiFi, show IP + Companion IP briefly
  setTextNow("IP " + WiFi.localIP().toString());
  delay(2000);

  setTextNow("Companion IP " + String(companion_host));
  delay(2000);

  // Apply initial brightness
  applyBrightness();

  Serial.println("[System] Setup complete, entering loop");
}

// ------------------------ LOOP -----------------------------

void loop() {
  M5.update();
  unsigned long now = millis();

  // Attempt Companion connection if not connected
  if (!client.connected() && (now - lastConnectTry >= connectRetryMs)) {
    lastConnectTry = now;

    Serial.print("[NET] Connecting to Companion ");
    Serial.print(companion_host);
    Serial.print(":");
    Serial.println(companion_port);

    if (client.connect(companion_host, atoi(companion_port))) {
      Serial.println("[NET] Connected to Companion API");
      setTextNow("Connected");
      delay(500);
      sendAddDevice();
      lastPingTime = millis();
      setTextNow("Waiting...");
    } else {
      Serial.println("[NET] Companion connect failed");
      // NO config portal here – just keep retrying
    }
  }

  // Handle Companion traffic
  if (client.connected()) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) {
        parseAPI(line);
      }
    }

    // Periodic PING
    if (now - lastPingTime >= pingIntervalMs) {
      client.println("PING m5stickc");
      lastPingTime = now;
    }
  }

  // Front button -> KEY-PRESS
  if (M5.BtnA.wasPressed()) {
    sendKeyPress(true);
  }
  if (M5.BtnA.wasReleased()) {
    sendKeyPress(false);
  }

  // Handle scrolling animation
  if (scrolling && (now - lastScrollTime >= scrollIntervalMs)) {
    lastScrollTime = now;
    scrollX--;
    if (scrollX < -scrollWidth) {
      scrollX = SCREEN_W;
    }
    refreshDisplay();
  }
}
