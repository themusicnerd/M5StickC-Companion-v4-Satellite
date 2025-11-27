# M5StickC Companion v4 Single-Button Satellite

A compact, **single-button satellite surface** for **Bitfocus Companion v4**, built on the **M5StickC** (v0.3.0 library).

This little box gives you:

- A single physical button that talks to Companion’s Satellite API  
- A bright text display that:
  - Shows ultra-large labels (perfect for “GO”, “REC”, “A”, “B”)
  - Handles multi-line text
  - Falls back to a smooth scrolling marquee for longer labels
- Colour feedback (background + text) from Companion  
- Configurable Companion IP/port via WiFiManager config portal  
- Automatic, MAC-based `DEVICEID` so it’s plug-and-play on any unit

Perfect for **on-stage triggers**, **countdown/info display**, **simple tally/“live” indicators**, or a super-portable “one key I can always trust” surface.

---

## Features

### Display & Text Logic

- Uses the built-in M5StickC TFT in **landscape (160x80)**.
- Text modes:
  - **Ultra Big**:  
    - `<= 3` characters → `textSize = 6`  
    - Ideal for “A”, “B”, “GO”, “REC”, etc.
  - **Big**:  
    - `4–6` characters → `textSize = 4`
  - **Normal / Info Mode**:  
    - `> 6` characters → `textSize = 2`
    - Tries to word-wrap into **up to 3 lines**.
    - If it still doesn’t fit → switches to **scrolling marquee**.
- Scrolling:
  - Single-line horizontal scroll at the **bottom of the screen**.
  - Smooth scroll with a 50 ms step.

### Colour & Brightness

- Responds to Companion `KEY-STATE` parameters:
  - `COLOR` → background colour  
  - `TEXTCOLOR` → text colour  
- Colours can be sent as:
  - `#RRGGBB`
  - `"#RRGGBB"`
  - `R,G,B`
- Brightness:
  - Listens to `BRIGHTNESS VALUE=<0–100>` from Companion.
  - Maps:
    - `0` → screen off (`ScreenBreath(0)`)
    - `1–100` → `ScreenBreath(7–50)` for a useful brightness range.

### Button Behaviour

- Uses **BtnA** on the M5StickC.
- Sends Companion v4 Satellite API messages:
  - On press: `KEY-PRESS ... PRESSED=true`
  - On release: `KEY-PRESS ... PRESSED=false`

### Companion v4 Integration

- Connects via **TCP** to Companion’s **Satellite API**.
- On connect sends:
  - `ADD-DEVICE DEVICEID=<DEVICEID> PRODUCT_NAME="M5StickC" KEYS_TOTAL=1 KEYS_PER_ROW=1 BITMAPS=0 COLORS=true TEXT=true`
- `DEVICEID` is:
  - `M5StickC_<MAC>` (MAC with no colons, uppercased)
  - Example: `M5StickC_2CF43276D339`
- Expects:
  - `KEY-STATE` lines containing `TEXT`, `COLOR`, `TEXTCOLOR`, `BRIGHTNESS`.
  - `TEXT` is base64 encoded by Companion; the device decodes it.

---

## Hardware & Requirements

- **M5StickC** (original, not Plus).
- Arduino toolchain with:
  - **ESP32 board package** installed.
  - **M5StickC library v0.3.0**.
  - **WiFiManager** library.
  - Standard ESP32 `WiFi` and `EEPROM` libs.

No extra wiring is required – everything is in the Stick:
- Onboard TFT for UI
- BtnA for trigger
- WiFi for Companion link

---

## WiFi & Config Portal

### Stored Settings

The device stores in EEPROM:

- Companion host (IP or hostname)
- Companion port (default `16622`)
- A boot counter (used for safeties / forced config)

### Normal Boot Flow

1. Power up → shows **`BOOT`** briefly.
2. Shows **`CONFIG?`** for ~5 seconds:
   - If you **press BtnA during this window**, it will:
     - Jump straight into WiFi config portal.
3. Device connects via WiFi using stored WiFiManager credentials.
4. On success it briefly shows:
   - `IP <your-ip>`
   - `Companion IP <stored-host>`
5. Then it connects to Companion and displays your button text.

### Entering Config Portal

**Two ways:**

1. **At Boot (Manual)**
   - When "CONFIG?" is displayed, press **BtnA**.
   - The device starts its own AP and config portal.

2. **WiFi Failure / Boot Counter**
   - If previous boots have failed / boot counter passes threshold, it automatically enters the config portal.

### Portal Details

- AP SSID: `M5StickC_<MAC>`
- AP password: *(blank – open)*
- Portal URL: `http://192.168.4.1/`

From the portal you can set:

- **Companion IP**  
- **Satellite Port**  
- (Optional) **Boot Counter** (for debugging)

On save:

- Values are written to EEPROM.
- Boot counter is reset.
- A brief `CFG SAVED` message is shown.
- Device reboots and connects normally.

---

## Companion v4 Setup

1. In Companion, enable the **Satellite TCP API** (port `16622` by default).
2. Power up the M5StickC and let it connect to WiFi & Companion.
3. In Companion, you should see a new device with something like:
   - `M5StickC_2CF43276D339`
4. Assign actions/feedback to that device’s `KEY 0`:
   - **TEXT** feedback:
     - This becomes the label displayed on the Stick.
   - **COLOR** / **TEXTCOLOR**:
     - These drive background and text colour.
   - **BRIGHTNESS**:
     - Control screen brightness 0–100.

You now have a tiny, robust **single-key satellite surface** talking natively to Companion v4.

---

## Text Behaviour Summary

- `len(text) <= 3`  
  → **Ultra Big Mode** (`textSize = 6`), centered.

- `4 <= len(text) <= 6`  
  → **Big Mode** (`textSize = 4`), centered.

- `len(text) > 6`  
  → Try word-wrap into up to **3 centered lines** (`textSize = 2`).  
  → If still too long → **scrolling marquee** at the bottom.

---

## Building & Flashing

1. Open the `.ino` file in **Arduino IDE**.
2. Select:
   - Board: **ESP32 Dev Module** (or equivalent known-good profile for M5StickC).
   - Flash Frequency / Partition: defaults usually fine.
3. Install dependencies:
   - `M5StickC` library (v0.3.0).
   - `WiFiManager`.
4. Plug in your M5StickC via USB-C.
5. Choose the correct **COM port**.
6. Click **Upload**.

On first boot:

- Join the `M5StickC_<MAC>` AP,
- Open `http://192.168.4.1/`,
- Configure:
  - Your WiFi SSID/Password,
  - Companion IP,
  - Companion Satellite Port (default `16622`).

---

## Use Cases

- **On-stage “GO” / “NEXT” trigger**
- **Camera or scene tally / live indicator**
- **Quick “Record armed” feedback**
- **Countdown labels / simple show states**

Because the text/colour/brightness are driven entirely from Companion, you can reuse the same physical device for multiple shows with different layouts.

---

## License

MIT (or your preferred license).

---

## Credits

- Hardware: **M5StickC**
- Control system: **Bitfocus Companion v4**
