/*
  ------------------------------------------------------------
  M5StickC / M5StickCPlus Companion v4
  Single Button Satellite (Bitmap + Text Mode)
  Author: Adrian Davis
  URL: https://github.com/themusicnerd/M5-AtomS3-Companion-v4-Satellite
  License: MIT

  Features:
    - Companion v4 Satellite API support
    - Single-button surface + external RGB LED
    - Two display modes (WiFi-configurable):
        * BITMAP  : uses 72x72 Companion bitmaps (RGB/RGBA) upscaled to screen
        * TEXT    : Companion TEXT (base64) rendered with:
            - Extra-big (≤2 chars)   : huge font, centered
            - Ultra-large (3 chars)  : very big font, centered
            - Large (4–6 chars)      : big font, centered
            - Normal (>6 chars)      : multi-line, centered (auto-wrap or manual "\n")
        * Uses COLOR/TEXTCOLOR for background/text in TEXT mode
        * Text Rotate (0/90/180/270 in WiFiManager, TEXT mode only)
        * Yellow 4px border when button is pressed (TEXT mode only)
    - External RGB LED PWM output (common cathode G0, RED=G36, GREEN=G26, BLUE reserved)
    - WiFiManager config portal (hold BtnA for 5s)
    - OTA firmware updates
    - Full MAC-based deviceID (M5StickC_<MAC>)
    - Auto reconnect, ping, and key-release failsafe

  Target boards:
    - M5StickC
    - M5StickCPlus
  ------------------------------------------------------------
*/

#include <M5Unified.h>
#include <M5GFX.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <memory>
#include <mbedtls/base64.h>
#include <vector>
#include <math.h>
#include <esp32-hal-ledc.h>

// ------------------------------------------------------------
// Board-specific defaults
// ------------------------------------------------------------

// Default bitmap rotation for StickC / StickCPlus
#if defined(ARDUINO_M5STICK_C)      || \
    defined(ARDUINO_M5Stick_C)      || \
    defined(ARDUINO_M5STACK_M5STICKC_PLUS) || \
    defined(ARDUINO_M5STICKC_PLUS)
// Landscape with USB/power button on the right
const int DEFAULT_BITMAP_ROTATION = 1;
#else
// Fallback (if compiled for some other ESP32 board)
const int DEFAULT_BITMAP_ROTATION = 1;
#endif

// ------------------------------------------------------------
// Globals / Config
// ------------------------------------------------------------

Preferences preferences;
WiFiManager wifiManager;
WiFiClient  client;

// Companion server
char companion_host[40] = "Companion IP";
char companion_port[6]  = "16622";

// Static IP (0.0.0.0 = DHCP)
IPAddress stationIP   = IPAddress(0, 0, 0, 0);
IPAddress stationGW   = IPAddress(0, 0, 0, 0);
IPAddress stationMask = IPAddress(255, 255, 255, 0);

// Device ID – full MAC will be appended
String deviceID = "";

// AP password for config portal (empty = open)
const char* AP_password = "";

// Timing
unsigned long lastPingTime    = 0;
unsigned long lastConnectTry  = 0;
const unsigned long connectRetryMs = 5000;
const unsigned long pingIntervalMs = 2000;

// Brightness (0–100)
int brightness = 100;

// External RGB LED (Jaycar RGB LED) --------------------------
// Common cathode on G0, Red on G36, Green on G26.
// Blue is reserved for future use and currently disabled.
const int LED_PIN_GND   = 0;    // G0 common cathode
const int LED_PIN_RED   = 36;   // G36 red
const int LED_PIN_GREEN = 26;   // G26 green
const int LED_PIN_BLUE  = -1;   // reserved / disabled for now

const uint32_t pwmFreq       = 5000;
const uint8_t  pwmResolution = 8;

uint8_t lastColorR = 0;
uint8_t lastColorG = 0;
uint8_t lastColorB = 0;
// ------------------------------------------------------------

// Display mode
enum DisplayMode {
  DISPLAY_BITMAP = 0,
  DISPLAY_TEXT   = 1
};
int displayMode = DISPLAY_BITMAP; // default

// WiFiManager custom params
WiFiManagerParameter* custom_companionIP;
WiFiManagerParameter* custom_companionPort;
WiFiManagerParameter* custom_displayMode;
WiFiManagerParameter* custom_rotation = nullptr;   // rotation parameter
int screenRotation = 0;  // 0 = 0°, 1 = 90°, 2 = 180°, 3 = 270° (TEXT mode only)

// TEXT mode pressed-border flag (yellow outline when button pressed)
bool textPressedBorder = false;

// Forward declarations for text mode / brightness
void setText(const String& txt);
void applyDisplayBrightness();

// ------------------------------------------------------------
// Simple logger
// ------------------------------------------------------------
void logger(const String& s, const String& type = "info") {
  Serial.println(s);
}

String getParam(const String& name) {
  if (wifiManager.server && wifiManager.server->hasArg(name))
    return wifiManager.server->arg(name);
  return "";
}

// ------------------------------------------------------------
// Save WiFiManager params -> Preferences
// ------------------------------------------------------------
void saveParamCallback() {
  String str_companionIP   = getParam("companionIP");
  String str_companionPort = getParam("companionPort");
  String str_displayMode   = getParam("displayMode");
  String str_rotation      = getParam("rotation");   // rotation (0/90/180/270)

  preferences.begin("companion", false);
  if (str_companionIP.length() > 0)    preferences.putString("companionip",   str_companionIP);
  if (str_companionPort.length() > 0)  preferences.putString("companionport", str_companionPort);
  if (str_displayMode.length() > 0)    preferences.putString("displayMode",   str_displayMode);
  if (str_rotation.length() > 0)       preferences.putString("rotation",      str_rotation);
  preferences.end();
}

// ------------------------------------------------------------
// External LED Handling (LEDC API: ledcAttach / ledcWrite)
// ------------------------------------------------------------
void setExternalLedColor(uint8_t r, uint8_t g, uint8_t b) {
  lastColorR = r;
  lastColorG = g;
  lastColorB = b;

  // For *common cathode* (LED GND to LED_PIN_GND):
  uint8_t scaledR = r * max(brightness, 15) / 100;
  uint8_t scaledG = g * max(brightness, 15) / 100;
  uint8_t scaledB = b * max(brightness, 15) / 100;

  // For *common anode* (LED VCC to +3V3), uncomment below:
  // scaledR = 255 - scaledR;
  // scaledG = 255 - scaledG;
  // scaledB = 255 - scaledB;

  Serial.print("[LED] setExternalLedColor raw r/g/b = ");
  Serial.print(r); Serial.print("/");
  Serial.print(g); Serial.print("/");
  Serial.print(b);
  Serial.print("  scaled = ");
  Serial.print(scaledR); Serial.print("/");
  Serial.print(scaledG); Serial.print("/");
  Serial.println(scaledB);

  if (LED_PIN_RED   >= 0) ledcWrite(LED_PIN_RED,   scaledR);
  if (LED_PIN_GREEN >= 0) ledcWrite(LED_PIN_GREEN, scaledG);
  if (LED_PIN_BLUE  >= 0) ledcWrite(LED_PIN_BLUE,  scaledB);  // currently disabled
}

// ------------------------------------------------------------
// Display Helpers (shared / boot messages)
// ------------------------------------------------------------
void clearScreen(uint16_t color = BLACK) {
  M5.Display.fillScreen(color);
}

void drawCenterText(const String& txt, uint16_t color = WHITE, uint16_t bg = BLACK) {
  M5.Display.fillScreen(bg);
  M5.Display.setFont(&fonts::Font0);   // small, clean status font
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(color, bg);
  M5.Display.setTextDatum(middle_center);

  // Split into lines
  std::vector<String> lines;
  int start = 0;
  while (true) {
    int idx = txt.indexOf('\n', start);
    if (idx < 0) {
      lines.push_back(txt.substring(start));
      break;
    }
    lines.push_back(txt.substring(start, idx));
    start = idx + 1;
  }

  int lineHeight  = M5.Display.fontHeight();
  int totalHeight = lineHeight * lines.size();
  int topY        = (M5.Display.height() - totalHeight) / 2 + (lineHeight / 2);

  for (int i = 0; i < (int)lines.size(); i++) {
    int y = topY + i * lineHeight;
    M5.Display.drawString(lines[i], M5.Display.width() / 2, y);
  }
}

// ------------------------------------------------------------
// BITMAP MODE: Draw 72x72 RGB888 / RGBA upscaled to full screen
// ------------------------------------------------------------
void drawBitmapRGB888FullScreen(uint8_t* rgb, int size) {
  int sw = M5.Display.width();
  int sh = M5.Display.height();

  for (int y = 0; y < sh; y++) {
    int srcY = (y * size) / sh;
    for (int x = 0; x < sw; x++) {
      int srcX = (x * size) / sw;
      int idx = (srcY * size + srcX) * 3;
      M5.Display.drawPixel(x, y, M5.Display.color565(rgb[idx], rgb[idx+1], rgb[idx+2]));
    }
  }
}

// ------------------------------------------------------------
// TEXT MODE: Globals and Helpers
// ------------------------------------------------------------

// Base64 table for text decoding
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

// Text layout state (no scrolling anymore)
String currentText  = "";
String line1        = "";
String line2        = "";
String line3        = "";
int    numLines     = 0;                 // 0..X for auto-wrap
const int MAX_AUTO_LINES = 7;           // Auto-wrap up to this amount of lines

// Manual line handling (\n)
std::vector<String> manualLines;
bool useManualLines = false;

uint16_t bgColor   = BLACK;
uint16_t txtColor  = WHITE;

// Apply Companion brightness (0–100) to display
void applyDisplayBrightness() {
  int p = brightness;
  if (p < 0)   p = 0;
  if (p > 100) p = 100;
  uint8_t level = map(p, 0, 100, 0, 255);
  M5.Display.setBrightness(level);
}

// Decode Companion TEXT (base64 → UTF-8 or pass-through)
String decodeCompanionText(const String& encoded) {
  if (encoded.length() == 0) return encoded;

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

  if (!looksBase64) return encoded;

  String decoded = decodeBase64(encoded);
  if (decoded.length() == 0) return encoded;
  return decoded;
}

// Parse COLOR=/TEXTCOLOR= in forms:
//   #RRGGBB
//   R,G,B
//   rgb(r,g,b)
//   rgba(r,g,b,a)
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

  // Strip surrounding quotes if present
  if (val.length() >= 2 && val[0] == '\"' && val[val.length() - 1] == '\"') {
    val = val.substring(1, val.length() - 1);
  }

  // rgb()/rgba()
  if (val.startsWith("rgb(") || val.startsWith("rgba(")) {
    String c = val;
    c.replace("rgba(", "");
    c.replace("rgb(", "");
    c.replace(")", "");
    c.replace(" ", "");
    int p1 = c.indexOf(',');
    int p2 = c.indexOf(',', p1+1);
    if (p1 < 0 || p2 < 0) return false;
    int p3 = c.indexOf(',', p2+1);  // may or may not exist (alpha)

    r = c.substring(0, p1).toInt();
    g = c.substring(p1+1, p2).toInt();
    if (p3 >= 0) {
      b = c.substring(p2+1, p3).toInt();
    } else {
      b = c.substring(p2+1).toInt();
    }
    return true;
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

// ------------------------------------------------------------
// Font helpers for TEXT mode (all full ASCII)
// ------------------------------------------------------------
void setExtraBigFont() {
  // Extra big for ≤2 chars
  M5.Display.setFont(&fonts::Font8);
  M5.Display.setTextSize(1);   // really chunky
}

void setUltraFont() {
  // Ultra-large for 3 chars
  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextSize(2);
}

void setLargeFont() {
  // Large for 4–6 chars
  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextSize(2);
}

void setNormalFont() {
  // Normal font for multi-line
  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextSize(1);
}

// ------------------------------------------------------------
// Word-wrap into up to MAX_AUTO_LINES lines
// (font must already be set before calling)
// ------------------------------------------------------------
bool wrapToLines(const String& src, String& l1, String& l2, String& l3, int& outLines) {
  l1 = "";
  l2 = "";
  l3 = "";
  outLines = 0;

  if (src.length() == 0) return true;

  M5.Display.setTextWrap(false);

  int screenW = M5.Display.width();

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

  String linesBuf[MAX_AUTO_LINES];
  String* lines[MAX_AUTO_LINES];
  for (int i = 0; i < MAX_AUTO_LINES; i++) {
    linesBuf[i] = "";
    lines[i] = &linesBuf[i];
  }

  int currentLine = 0;

  for (size_t i = 0; i < words.size(); i++) {
    if (currentLine >= MAX_AUTO_LINES) {
      return false;
    }

    String candidate;
    if (lines[currentLine]->length() == 0) {
      candidate = words[i];
    } else {
      candidate = *lines[currentLine] + " " + words[i];
    }

    int w = M5.Display.textWidth(candidate);

    if (w <= screenW) {
      *lines[currentLine] = candidate;
    } else {
      currentLine++;
      if (currentLine >= MAX_AUTO_LINES) {
        return false;
      }
      *lines[currentLine] = words[i];
    }
  }

  outLines = currentLine + 1;

  if (outLines >= 1) l1 = linesBuf[0];
  if (outLines >= 2) l2 = linesBuf[1];
  if (outLines >= 3) l3 = linesBuf[2];

  return true;
}

// ------------------------------------------------------------
// Draw yellow border when button is pressed in TEXT mode
// ------------------------------------------------------------
void drawTextPressedBorderIfNeeded() {
  if (!textPressedBorder) return;

  int w = M5.Display.width();
  int h = M5.Display.height();
  uint16_t borderColor = M5.Display.color565(255, 255, 0); // yellow

  // 4px thick border using nested rectangles
  for (int i = 0; i < 4; i++) {
    M5.Display.drawRect(i, i, w - i * 2, h - i * 2, borderColor);
  }
}

// ------------------------------------------------------------
// Decide layout based on currentText (no scrolling)
// ------------------------------------------------------------
void analyseLayout() {
  line1 = "";
  line2 = "";
  line3 = "";
  numLines = 0;
  useManualLines = false;
  manualLines.clear();

  int len = currentText.length();
  if (len == 0) return;

  // -------------- Manual \n lines --------------
  if (currentText.indexOf('\n') >= 0) {
    setNormalFont();
    M5.Display.setTextWrap(false);

    int start = 0;
    while (true) {
      int idx = currentText.indexOf('\n', start);
      if (idx < 0) {
        manualLines.push_back(currentText.substring(start));
        break;
      }
      manualLines.push_back(currentText.substring(start, idx));
      start = idx + 1;
    }

    // Limit to 5 visible lines
    if ((int)manualLines.size() > 5) {
      manualLines.resize(5);
    }

    useManualLines = true;
    return;
  }

  // -------------- Extra-big / Ultra / Large single-line modes --------------
  if (len <= 2) {
    setExtraBigFont();
    numLines = 1;
    line1    = currentText;
    return;
  }

  if (len == 3) {
    setUltraFont();
    numLines = 1;
    line1    = currentText;
    return;
  }

  if (len <= 6) {
    setLargeFont();
    numLines = 1;
    line1    = currentText;
    return;
  }

  // -------------- Normal text: auto wrap, no scrolling fallback ----------
  setNormalFont();
  M5.Display.setTextWrap(false);

  String w1, w2, w3;
  int    lines = 0;
  bool   fits  = wrapToLines(currentText, w1, w2, w3, lines);

  if (fits && lines > 0 && lines <= MAX_AUTO_LINES) {
    numLines  = lines;
    line1     = w1;
    line2     = (lines >= 2) ? w2 : "";
    line3     = (lines >= 3) ? w3 : "";
    return;
  }

  // If wrapping somehow fails, just show it as a single line with normal font
  numLines = 1;
  line1    = currentText;
}

// ------------------------------------------------------------
// Repaint display based on text layout state (no scrolling)
// ------------------------------------------------------------
void refreshTextDisplay() {
  M5.Display.fillScreen(bgColor);
  M5.Display.setTextColor(txtColor, bgColor);
  M5.Display.setTextWrap(false);

  if (currentText.length() == 0) {
    // Still draw border if requested (e.g. pressed but no text)
    drawTextPressedBorderIfNeeded();
    return;
  }

  int screenW = M5.Display.width();
  int screenH = M5.Display.height();

  // Manual multi-line mode using \n (up to X lines)
  if (useManualLines) {
    setNormalFont();
    M5.Display.setTextDatum(middle_center);

    int lineHeight  = M5.Display.fontHeight();
    int lines       = manualLines.size();
    int totalHeight = lineHeight * lines;
    int topY        = (screenH - totalHeight) / 2 + (lineHeight / 2);

    for (int i = 0; i < lines; i++) {
      int y = topY + i * lineHeight;
      M5.Display.drawString(manualLines[i], screenW / 2, y);
    }

    drawTextPressedBorderIfNeeded();
    return;
  }

  M5.Display.setTextDatum(middle_center);

  int len = currentText.length();

  // Extra-big single-line (≤2 chars)
  if (len <= 2) {
    setExtraBigFont();
    M5.Display.drawString(currentText, screenW / 2, screenH / 2);
    drawTextPressedBorderIfNeeded();
    return;
  }

  // Ultra-large single-line (3 chars)
  if (len == 3) {
    setUltraFont();
    M5.Display.drawString(currentText, screenW / 2, screenH / 2);
    drawTextPressedBorderIfNeeded();
    return;
  }

  // Large single-line (4–6 chars)
  if (len <= 6) {
    setLargeFont();
    M5.Display.drawString(currentText, screenW / 2, screenH / 2);
    drawTextPressedBorderIfNeeded();
    return;
  }

  // Static multi-line centered mode (auto-wrap)
  if (numLines >= 1 && numLines <= MAX_AUTO_LINES) {
    setNormalFont();
    String lines[3] = { line1, line2, line3 };  // we only stored first 3

    int lineHeight  = M5.Display.fontHeight();
    int totalHeight = lineHeight * numLines;
    int topY        = (screenH - totalHeight) / 2 + (lineHeight / 2);

    for (int i = 0; i < numLines && i < 3; i++) {
      int y = topY + i * lineHeight;
      M5.Display.drawString(lines[i], screenW / 2, y);
    }
  }

  // Finally, overlay yellow border if button is pressed
  drawTextPressedBorderIfNeeded();
}

// ------------------------------------------------------------
// Update text and redraw immediately
// ------------------------------------------------------------
void setTextNow(const String& txt) {
  currentText = txt;
  analyseLayout();
  refreshTextDisplay();
}

void setText(const String& txt) {
  setTextNow(txt);
}

// ------------------------------------------------------------
// Handle TEXT= in KEY-STATE for TEXT mode
// ------------------------------------------------------------
void handleKeyStateTextField(const String& line) {
  int tPos = line.indexOf("TEXT=");
  if (tPos < 0) return;

  int firstQuote = line.indexOf('\"', tPos);
  if (firstQuote < 0) return;

  int secondQuote = line.indexOf('\"', firstQuote + 1);
  if (secondQuote < 0) return;

  String textField = line.substring(firstQuote + 1, secondQuote);
  String decoded   = decodeCompanionText(textField);
  decoded.replace("\\n", "\n");

  Serial.print("[API] TEXT encoded=\"");
  Serial.print(textField);
  Serial.print("\" decoded=\"");
  Serial.print(decoded);
  Serial.println("\"");

  setText(decoded);
}

// ------------------------------------------------------------
// Handle COLOR/TEXTCOLOR for TEXT mode
// ------------------------------------------------------------
void handleTextModeColors(const String& line) {
  int r, g, b;
  bool bgOk  = false;
  bool txtOk = false;

  if (parseColorToken(line, "COLOR", r, g, b)) {
    bgColor = M5.Display.color565(r, g, b);
    bgOk = true;
    Serial.printf("[API] TEXT BG COLOR r=%d g=%d b=%d\n", r, g, b);
  }

  if (parseColorToken(line, "TEXTCOLOR", r, g, b)) {
    txtColor = M5.Display.color565(r, g, b);
    txtOk = true;
    Serial.printf("[API] TEXT FG COLOR r=%d g=%d b=%d\n", r, g, b);
  }

  if (bgOk || txtOk) {
    refreshTextDisplay();
  }
}

// ------------------------------------------------------------
// Companion / Satellite API
// ------------------------------------------------------------
void sendAddDevice() {
  String cmd;

  if (displayMode == DISPLAY_TEXT) {
    // TEXT-ONLY mode: no bitmaps, just TEXT + COLORS
    cmd = "ADD-DEVICE DEVICEID=" + deviceID +
          " PRODUCT_NAME=\"M5StickC/Plus (TEXT)\" "
          "KEYS_TOTAL=1 KEYS_PER_ROW=1 "
          "COLORS=rgb TEXT=true BITMAPS=0";
  } else {
    // BITMAP mode: bitmaps enabled, no TEXT
    cmd = "ADD-DEVICE DEVICEID=" + deviceID +
          " PRODUCT_NAME=\"M5StickC/Plus (BITMAP)\" "
          "KEYS_TOTAL=1 KEYS_PER_ROW=1 "
          "COLORS=rgb TEXT=false BITMAPS=72";
  }

  client.println(cmd);
  Serial.println("[API] Sent: " + cmd);
}

// Handle KEY-STATE for both modes
void handleKeyState(const String& line) {
  Serial.println("[API] KEY-STATE line: " + line);

  // -------- COLOR=rgba(...) for LED (shared by both modes) --------
  int colorPos = line.indexOf("COLOR=");
  if (colorPos >= 0) {
    int start = colorPos + 6;
    int end = line.indexOf(' ', start);
    if (end < 0) end = line.length();
    String c = line.substring(start, end);
    c.trim();

    Serial.println("[API] COLOR raw: " + c);

    if (c.startsWith("\"") && c.endsWith("\""))
      c = c.substring(1, c.length() - 1);

    if (c.startsWith("rgba(") || c.startsWith("rgb(")) {
      String s = c;
      s.replace("rgba(", "");
      s.replace("rgb(", "");
      s.replace(")", "");
      s.replace(" ", "");
      int p1 = s.indexOf(',');
      int p2 = s.indexOf(',', p1+1);
      int p3 = s.indexOf(',', p2+1);
      int r = s.substring(0, p1).toInt();
      int g = s.substring(p1+1, p2).toInt();
      int b;
      if (p3 >= 0) {
        b = s.substring(p2+1, p3).toInt();
      } else {
        b = s.substring(p2+1).toInt();
      }

      Serial.print("[API] Parsed LED COLOR r/g/b = ");
      Serial.print(r); Serial.print("/");
      Serial.print(g); Serial.print("/");
      Serial.println(b);

      setExternalLedColor(r, g, b);
    } else {
      Serial.println("[API] COLOR is not rgba()/rgb(), ignoring for LED.");
    }
  } else {
    Serial.println("[API] No COLOR= field in KEY-STATE for LED.");
  }

  // -------- Display handling by mode --------
  if (displayMode == DISPLAY_BITMAP) {
    // BITMAP mode – ignore TEXT, focus on BITMAP=
    int bmpPos = line.indexOf("BITMAP=");
    if (bmpPos >= 0) {
      int start = bmpPos + 7;
      int end = line.indexOf(' ', start);
      if (end < 0) end = line.length();
      String bmp = line.substring(start, end);
      bmp.trim();

      if (bmp.startsWith("\"") && bmp.endsWith("\""))
        bmp = bmp.substring(1, bmp.length() - 1);

      int inLen = bmp.length();
      if (inLen <= 0) {
        Serial.println("[API] BITMAP present but empty.");
        return;
      }

      Serial.println("[API] BITMAP base64 length: " + String(inLen));

      size_t out_max = (inLen * 3) / 4 + 4;
      std::unique_ptr<uint8_t[]> buf(new uint8_t[out_max]);
      size_t out_len = 0;

      int res = mbedtls_base64_decode(buf.get(), out_max, &out_len,
                                      (const unsigned char*)bmp.c_str(), inLen);
      if (res != 0) {
        Serial.println("[API] Base64 decode failed, res=" + String(res) + " out_len=" + String(out_len));
        return;
      }

      Serial.println("[API] Decoded BITMAP bytes: " + String(out_len));

      int sizeRGB  = sqrt(out_len / 3);
      int sizeRGBA = sqrt(out_len / 4);

      bool isRGB  = (sizeRGB  * sizeRGB  * 3 == (int)out_len);
      bool isRGBA = (sizeRGBA * sizeRGBA * 4 == (int)out_len);

      if (isRGB) {
        Serial.println("[API] BITMAP detected as RGB, size=" + String(sizeRGB));
        drawBitmapRGB888FullScreen(buf.get(), sizeRGB);
      } else if (isRGBA) {
        Serial.println("[API] BITMAP detected as RGBA, size=" + String(sizeRGBA));
        int pixels = sizeRGBA * sizeRGBA;
        std::unique_ptr<uint8_t[]> rgb(new uint8_t[pixels * 3]);
        uint8_t* s = buf.get();
        uint8_t* d = rgb.get();
        for (int i = 0; i < pixels; i++) {
          d[0] = s[0];
          d[1] = s[1];
          d[2] = s[2];
          s += 4;
          d += 3;
        }
        drawBitmapRGB888FullScreen(rgb.get(), sizeRGBA);
      } else {
        Serial.println("[API] BITMAP size mismatch, cannot infer square dimensions.");
      }
    }
  } else {
    // TEXT mode – ignore BITMAP, use TEXT + COLOR/TEXTCOLOR
    handleTextModeColors(line);
    handleKeyStateTextField(line);
  }
}

// ------------------------------------------------------------
// Parse all Companion API messages
// ------------------------------------------------------------
void parseAPI(const String& apiData) {
  if (apiData.length() == 0) return;
  if (apiData.startsWith("PONG"))   return;

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
      brightness = v.toInt();
      Serial.println("[API] BRIGHTNESS set to " + String(brightness));
      setExternalLedColor(lastColorR, lastColorG, lastColorB);
      applyDisplayBrightness();
    }
    return;
  }

  if (apiData.startsWith("KEYS-CLEAR")) {
    Serial.println("[API] KEYS-CLEAR received");
    setExternalLedColor(0,0,0);
    if (displayMode == DISPLAY_TEXT) {
      setText("");
    } else {
      clearScreen(BLACK);
    }
    return;
  }

  if (apiData.startsWith("KEY-STATE")) {
    handleKeyState(apiData);
    return;
  }
}

// ------------------------------------------------------------
// WiFi + Config Portal
// ------------------------------------------------------------
void connectToNetwork() {
  if (stationIP != IPAddress(0,0,0,0))
    wifiManager.setSTAStaticIPConfig(stationIP, stationGW, stationMask);

  WiFi.mode(WIFI_STA);

  // Build default displayMode string
  char modeBuf[8];
  if (displayMode == DISPLAY_TEXT) {
    snprintf(modeBuf, sizeof(modeBuf), "text");
  } else {
    snprintf(modeBuf, sizeof(modeBuf), "bitmap");
  }

  // Build default rotation string based on current screenRotation (0..3)
  char rotBuf[5];
  int rotDeg = 0;
  if      (screenRotation == 1) rotDeg = 90;
  else if (screenRotation == 2) rotDeg = 180;
  else if (screenRotation == 3) rotDeg = 270;
  else                          rotDeg = 0;
  snprintf(rotBuf, sizeof(rotBuf), "%d", rotDeg);

  custom_companionIP   = new WiFiManagerParameter("companionIP", "Companion IP", companion_host, 40);
  custom_companionPort = new WiFiManagerParameter("companionPort", "Satellite Port", companion_port, 6);
  custom_displayMode   = new WiFiManagerParameter("displayMode", "Display Mode (bitmap/text)", modeBuf, 8);
  custom_rotation      = new WiFiManagerParameter("rotation", "Text Rotation (0/90/180/270)", rotBuf, 4);

  wifiManager.addParameter(custom_companionIP);
  wifiManager.addParameter(custom_companionPort);
  wifiManager.addParameter(custom_displayMode);
  wifiManager.addParameter(custom_rotation);
  wifiManager.setSaveParamsCallback(saveParamCallback);

  std::vector<const char*> menu = { "wifi", "param", "info", "sep", "restart", "exit" };
  wifiManager.setMenu(menu);
  wifiManager.setClass("invert");
  wifiManager.setConfigPortalTimeout(120);

  bool res = wifiManager.autoConnect(deviceID.c_str(), AP_password);

  if (!res) {
    Serial.println("[WiFi] Failed to connect, showing WiFi ERR");
    drawCenterText("WiFi ERR", RED, BLACK);
  } else {
    Serial.println("[WiFi] Connected to AP, IP=" + WiFi.localIP().toString());
    drawCenterText("WiFi OK", GREEN, BLACK);
    delay(500);
  }

  // Update globals from the latest config values
  preferences.begin("companion", true);
  if (preferences.getString("companionip").length() > 0)
    preferences.getString("companionip").toCharArray(companion_host, sizeof(companion_host));

  if (preferences.getString("companionport").length() > 0)
    preferences.getString("companionport").toCharArray(companion_port, sizeof(companion_port));

  String modeStr = preferences.getString("displayMode", custom_displayMode->getValue());
  String savedRotStr  = preferences.getString("rotation",   custom_rotation->getValue());
  preferences.end();

  // Display mode (bitmap / text)
  if (modeStr.equalsIgnoreCase("text")) {
    displayMode = DISPLAY_TEXT;
  } else {
    displayMode = DISPLAY_BITMAP;
  }

  // Map rotation degrees -> M5 rotation index (0..3)
  rotDeg = savedRotStr.toInt();
  if      (rotDeg == 90)  screenRotation = 1;
  else if (rotDeg == 180) screenRotation = 2;
  else if (rotDeg == 270) screenRotation = 3;
  else                    screenRotation = 0;

  // Apply rotation:
  //   - TEXT mode   -> use screenRotation
  //   - BITMAP mode -> board default
  if (displayMode == DISPLAY_TEXT) {
    M5.Display.setRotation(screenRotation);
  } else {
    M5.Display.setRotation(DEFAULT_BITMAP_ROTATION);
  }

  Serial.print("[Config] Display mode set to: ");
  Serial.println(displayMode == DISPLAY_TEXT ? "TEXT" : "BITMAP");
  Serial.print("[Config] Text rotation degrees: ");
  Serial.println(rotDeg);
  Serial.print("[Config] Text rotation index: ");
  Serial.println(screenRotation);
}

// ------------------------------------------------------------
// SETUP
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n[M5StickC] Booting...");

  // Build deviceID from MAC every boot
  WiFi.mode(WIFI_STA);
  delay(100);

  uint8_t mac[6];
  WiFi.macAddress(mac);

  char macBuf[13];
  sprintf(macBuf, "%02X%02X%02X%02X%02X%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  deviceID = "M5StickC_";
  deviceID += macBuf;
  deviceID.toUpperCase();

  Serial.println("[ID] deviceID = " + deviceID);

  // Load preferences
  preferences.begin("companion", false);
  preferences.putString("deviceid", deviceID);

  if (preferences.getString("companionip").length() > 0)
    preferences.getString("companionip").toCharArray(companion_host, sizeof(companion_host));

  if (preferences.getString("companionport").length() > 0)
    preferences.getString("companionport").toCharArray(companion_port, sizeof(companion_port));

  String modeStr = preferences.getString("displayMode", "bitmap");
  String rotStr  = preferences.getString("rotation",   "0");   // default rotation 0°

  if (modeStr.equalsIgnoreCase("text")) {
    displayMode = DISPLAY_TEXT;
  } else {
    displayMode = DISPLAY_BITMAP;
  }

  // Map saved degrees -> rotation index (0..3)
  int rotDeg = rotStr.toInt();
  if      (rotDeg == 90)  screenRotation = 1;
  else if (rotDeg == 180) screenRotation = 2;
  else if (rotDeg == 270) screenRotation = 3;
  else                    screenRotation = 0;

  preferences.end();

  Serial.print("[Prefs] Companion IP: ");
  Serial.println(companion_host);
  Serial.print("[Prefs] Companion Port: ");
  Serial.println(companion_port);
  Serial.print("[Prefs] Display Mode: ");
  Serial.println(displayMode == DISPLAY_TEXT ? "TEXT" : "BITMAP");
  Serial.print("[Prefs] Text rotation degrees: ");
  Serial.println(rotDeg);
  Serial.print("[Prefs] Text rotation index: ");
  Serial.println(screenRotation);

  auto cfg = M5.config();
  M5.begin(cfg);

  // Apply initial rotation:
  //   - TEXT mode   -> use screenRotation
  //   - BITMAP mode -> board-specific default
  if (displayMode == DISPLAY_TEXT) {
    M5.Display.setRotation(screenRotation);
  } else {
    M5.Display.setRotation(DEFAULT_BITMAP_ROTATION);
  }

  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  applyDisplayBrightness();
  clearScreen(BLACK);

  String bootMsg =
    "BOOTING\n\n"
    "M5StickC\n"
    "Companion v4\n"
    "Single Button\n"
    "Satellite\n(" +
    String(displayMode == DISPLAY_TEXT ? "TEXT" : "BITMAP") + " MODE)";

  drawCenterText(bootMsg, WHITE, BLACK);

  // ---- External LED Setup ----
  if (LED_PIN_GND >= 0) {
    pinMode(LED_PIN_GND, OUTPUT);
    digitalWrite(LED_PIN_GND, LOW);
  }

  Serial.println("[LED] Initialising PWM pins with ledcAttach...");
  if (LED_PIN_RED >= 0 && ledcAttach(LED_PIN_RED, pwmFreq, pwmResolution)) {
    Serial.println("[LED] PWM attached to RED pin");
  } else if (LED_PIN_RED >= 0) {
    Serial.println("[LED] ERROR attaching PWM to RED pin");
  }
  if (LED_PIN_GREEN >= 0 && ledcAttach(LED_PIN_GREEN, pwmFreq, pwmResolution)) {
    Serial.println("[LED] PWM attached to GREEN pin");
  } else if (LED_PIN_GREEN >= 0) {
    Serial.println("[LED] ERROR attaching PWM to GREEN pin");
  }
  if (LED_PIN_BLUE >= 0 && ledcAttach(LED_PIN_BLUE, pwmFreq, pwmResolution)) {
    Serial.println("[LED] PWM attached to BLUE pin");
  }

  setExternalLedColor(0,0,0);

  // Quick LED colour test
  Serial.println("[LED] Running power-on colour test (R/G/B)...");
  setExternalLedColor(255, 0, 0);  delay(300);
  setExternalLedColor(0, 255, 0);  delay(300);
  setExternalLedColor(0, 0, 255);  delay(300);
  setExternalLedColor(0, 0, 0);

  WiFi.setHostname(deviceID.c_str());
  connectToNetwork();

  ArduinoOTA.setHostname(deviceID.c_str());
  ArduinoOTA.setPassword("companion-satellite");
  ArduinoOTA.begin();

  clearScreen(BLACK);
  String waitMsg =
    "Waiting For\n"
    "Companion\n"
    "on\n" +
    String(companion_host) + "\n" + String(companion_port) +
    "\n(M5StickC " + (displayMode == DISPLAY_TEXT ? String("TEXT") : String("BITMAP")) + " MODE)";

  drawCenterText(waitMsg, WHITE, BLACK);
  Serial.println("[System] Setup complete, entering main loop.");
}

// ------------------------------------------------------------
// LOOP
// ------------------------------------------------------------
void loop() {
  M5.update();
  ArduinoOTA.handle();

  // 5s long-press -> Config Portal
  if (M5.BtnA.pressedFor(5000)) {
    // Force a KEY RELEASE in Companion so it never stays “held”
    if (client.connected()) {
      Serial.println("[BTN] Long press, sending KEY RELEASE before config portal");
      client.println("KEY-PRESS DEVICEID=" + deviceID + " KEY=0 PRESSED=false");
    }
    delay(50);

    // Config portal message
    String msg =
      "CONFIG PORTAL\n\n"
      "Connect to WiFi:\n" +
      deviceID +
      "\nThen go to:\n"
      "192.168.4.1";

    drawCenterText(msg, WHITE, BLACK);

    while (wifiManager.startConfigPortal(deviceID.c_str(), AP_password)) {}
    ESP.restart();
  }

  unsigned long now = millis();

  // Companion connection / reconnect
  if (!client.connected() && (now - lastConnectTry >= connectRetryMs)) {
    lastConnectTry = now;
    Serial.print("[NET] Connecting to Companion ");
    Serial.print(companion_host);
    Serial.print(":");
    Serial.println(companion_port);

    if (client.connect(companion_host, atoi(companion_port))) {
      Serial.println("[NET] Connected to Companion API");
      drawCenterText("Connected", GREEN, BLACK);
      delay(200);
      sendAddDevice();
      lastPingTime = millis();
    } else {
      Serial.println("[NET] Companion connect failed");
    }
  }

  // Handle Companion traffic
  if (client.connected()) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) parseAPI(line);
    }

    // Short press -> KEY-PRESS true/false
    if (M5.BtnA.wasPressed()) {
      Serial.println("[BTN] Short press -> KEY=0 PRESSED=true");
      client.println("KEY-PRESS DEVICEID=" + deviceID + " KEY=0 PRESSED=true");

      // Show yellow border while pressed in TEXT mode
      if (displayMode == DISPLAY_TEXT) {
        textPressedBorder = true;
        refreshTextDisplay();  // redraw text + border
      }
    }

    if (M5.BtnA.wasReleased()) {
      Serial.println("[BTN] Release -> KEY=0 PRESSED=false");
      client.println("KEY-PRESS DEVICEID=" + deviceID + " KEY=0 PRESSED=false");

      // Remove border when released in TEXT mode
      if (displayMode == DISPLAY_TEXT) {
        textPressedBorder = false;
        refreshTextDisplay();  // redraw text without border
      }
    }

    // Periodic PING
    if (now - lastPingTime >= pingIntervalMs) {
      client.println("PING m5stickc");
      lastPingTime = now;
    }
  }

  delay(10);
}
