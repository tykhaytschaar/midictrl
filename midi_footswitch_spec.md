# WiFi MIDI Footswitch – Project Specification

Target device: Neural DSP **Nano Cortex**, controlled via MIDI from a footswitch interface, with on-device and web-based configuration.

---

## 1. Hardware

### 1.1 Components

The firmware supports two MCU targets, picked at build time (see `tools/build.sh` or the per-target VS Code build tasks):

- **ESP32 WROOM-32** (`esp32`) — MVP target. Cheaper, smaller flash (4 MB), no PSRAM, and a noticeably less-linear SAR ADC. GPIO budget is the tighter constraint here; the pinout has to share scarce general-purpose pins between 8 footswitches, the encoder, the TFT, the WS2812 chain, the MIDI UART, and the expression ADC, almost certainly using a couple of input-only pins (GPIO 34–39) with external pull-ups for some of the footswitches.
- **ESP32-S3 WROOM-1 N16R8** (`esp32s3`) — full-feature target. 16 MB flash, 8 MB Octal PSRAM (room for an LVGL framebuffer if we want one), better-calibrated ADC, native USB-OTG. The pinout below targets this module.

| Component | Notes |
|-----------|-------|
| ESP32-S3 module (e.g. ESP32-S3-WROOM-1 N16R8, on a DevKitC-1 board) | Main MCU for the full build: built-in WiFi/BLE, dual Xtensa core, 16 MB flash, 8 MB Octal PSRAM |
| ESP32 module (ESP32-WROOM-32, on a DevKitV1 board) | MVP MCU: dual Xtensa core, 520 KB SRAM, 4 MB flash, no PSRAM, classic ADC |
| 3.5" IPS TFT, ST7796, SPI, 480x320, no-touch | Main display |
| Rotary encoder (Bourns PEC11R or similar, integrated push button) | For on-device configuration |
| 8x SPST momentary footswitches | 2 bank + 5 program + 1 tap tempo |
| WS2812B / SK6812 LED chain (at least 5, optionally +2-3 status LEDs) | Program slot colors and status. Driven via RMT peripheral |
| Expression pedal input (TRS jack, typically 10k–25k log/lin pot) | Wired to an ADC1 channel (ADC2 cannot be used while WiFi is active) |
| MIDI TRS Type A output (instead of 5-pin DIN) | Feeds the Nano Cortex input |
| 9V DC power supply (from pedalboard PSU) | Buck converter to 5V for USB or 3.3V LDO for the module's 3V3 rail |

### 1.2 Suggested GPIO pinout (ESP32-S3)

The pinout below assumes an ESP32-S3-WROOM-1 N16R8 module. Pins 26–32 are used by the internal SPI flash, and 33–37 by the Octal PSRAM, so they are not available. Pins 0, 3, 45, 46 are strapping pins and must be avoided for footswitches or anything that could be held in an unexpected state at boot. Pins 19/20 are reserved for native USB (USB-OTG D-/D+). ADC2 cannot be used while WiFi is active, so the expression pedal must use ADC1 (GPIO 1–10).

> The ESP32 WROOM-32 (MVP) variant of this table is **TODO** — the classic ESP32 has fewer freely usable GPIOs, several pins are strap- or flash-reserved, and GPIO 34–39 are input-only without internal pull-ups, so the WROOM pinout will need either external pull-ups on a few footswitches or an I²C port expander. Locked in before the first hardware revision.

| GPIO | Function |
|------|----------|
| GPIO0 | Boot strap (reserved, do not use) |
| GPIO1 | ADC1_CH0 → Expression pedal wiper |
| GPIO2 | spare (ADC1_CH1) |
| GPIO3 | strap (reserved) |
| GPIO4 | Footswitch: Bank Down |
| GPIO5 | Footswitch: Bank Up |
| GPIO6 | Footswitch: Program 1 |
| GPIO7 | Footswitch: Program 2 |
| GPIO8 | Footswitch: Program 3 |
| GPIO9 | Footswitch: Program 4 |
| GPIO10 | Footswitch: Program 5 |
| GPIO11 | Footswitch: Tap Tempo |
| GPIO12 | Encoder A |
| GPIO13 | Encoder B |
| GPIO14 | Encoder button |
| GPIO15 | WS2812 data (RMT TX channel) |
| GPIO16 | UART1 TX → MIDI out (via 220Ω to TRS tip) |
| GPIO17 | TFT DC |
| GPIO18 | TFT RST |
| GPIO19 | USB-OTG D- (reserved) |
| GPIO20 | USB-OTG D+ (reserved) |
| GPIO21 | TFT backlight (LEDC PWM) |
| GPIO26–32 | Reserved (internal SPI flash on the module) |
| GPIO33–37 | Reserved (Octal PSRAM on the -R8 variant) |
| GPIO38 | SPI2_HOST MOSI (TFT) |
| GPIO39 | SPI2_HOST SCK (TFT) |
| GPIO40 | SPI2_HOST CS (TFT) |
| GPIO41 | spare |
| GPIO42 | spare |
| GPIO43 | UART0 TX (USB-Serial console, reserved) |
| GPIO44 | UART0 RX (USB-Serial console, reserved) |
| GPIO45 | strap (reserved) |
| GPIO46 | strap (reserved) |

SPI2_HOST (formerly "HSPI") is used for the TFT; SPI3_HOST is left free. MISO is unused since the ST7796 is write-only in this design. All footswitches and the encoder are wired with the internal pull-up enabled and active low (GPIO → switch → GND).

### 1.3 MIDI output circuit

TRS Type A schematic (Nano Cortex input compatible):
- Tip → 220Ω → UART1 TX (GPIO16)
- Ring → 220Ω → 3.3V
- Sleeve → GND

Note: the Nano Cortex TRS MIDI input is Type A standard (tip = current source). The ESP32-S3 GPIO drive is 3.3V, which is within the MIDI 1.0 voltage tolerance. If the signal level proves marginal in practice, a 74HCT04 inverter buffer on 5V can level-shift it.

---

## 2. Functional Specification

### 2.1 Basic concepts

- **Bank**: a logical group of 5 program slots with its own name.
- **Program slot** (or preset): an action assigned to a footswitch that sends 1 or more MIDI messages. Has a name + message list + optional alt + optional expression target.
- **Current**: the currently active bank/program slot.
- **Target**: the currently selected but not yet committed bank (during browse mode).
- **Primary / Alternative state**: the two possible states of a slot, when an alt is configured.

### 2.2 Footswitch behavior

| Button | Short press | Long press |
|--------|-------------|------------|
| Bank Down | Decrease target bank (wraparound) | `user_function_a` (configurable MIDI message or NOOP) |
| Bank Up | Increase target bank (wraparound) | `user_function_b` (configurable MIDI message or NOOP) |
| Program 1–5 | Slot selection / alt toggle | (not defined yet, future hook) |
| Tap Tempo | Send tap tempo CC | Send tuner toggle CC |

Long press timings are globally configurable:
- `long_press_short_ms` (default 500 ms) – e.g. for tuner trigger
- `long_press_long_ms` (default 1500 ms) – e.g. for user function trigger

### 2.3 Bank navigation (Browse Mode)

Browse mode logic:
1. On boot, `current = target = 0` (or per `boot_resume`, the last state — only for `current`; `target` is always equal).
2. Short press of Bank Down/Up only modifies `target`. `current` is unchanged. No MIDI is sent.
3. While `target != current`, **browse mode** is active:
   - The main view of the display shows the **target bank** name (with or without the current program — see 4.1).
   - The 5 program LEDs **blink** in their own current color:
     - inactive slots in red
     - the currently active (current) slot in its own state color (green primary / blue alt)
4. Browse mode exit conditions:
   - **Commit**: short press of any Program 1–5 footswitch. Then `current := target`, the selected program slot is activated from primary, MIDI messages are sent.
   - **Implicit cancel**: if `target` becomes equal to `current` again from navigation (navigating back). Automatic exit.
   - **Timeout**: `browse_timeout_ms` (default 10000 ms, 0 = disabled). On expiry, `target := current`, exit.
5. After commit, every program slot's alt cache is cleared, all start in primary in the new bank (see 2.5).

### 2.4 Program slot selection

- Short press on a program button:
  - If `target != current` (browse mode): commit (see 2.3.4).
  - If browse mode is off **and** this is not the currently active slot: activate new slot in primary, send MIDI messages.
  - If browse mode is off **and** this is the currently active slot: alt toggle (see 2.5).
- Empty slot (empty `messages` list): activates (LED green, display shows its name or `EMPTY` text), but **no MIDI message is sent**. Alt has no meaning on it either.

### 2.5 Alternative program (Alt) toggle

Each slot may optionally have an alt object (own name + message list + optional expression target). Toggle behavior is a global setting (`alt_toggle_behavior`):

#### ALT_A mode (simpler)
- Only the **currently active** slot has a toggle state in memory.
- On selecting another slot or switching banks, the toggle state is cleared, the new slot starts in primary.

#### ALT_B mode (slot-level cache, within a bank)
- Every slot's toggle state is **remembered** within the bank, in RAM.
- When a slot is selected, it resumes from its last state (primary or alt).
- Example workflow:
  ```
  Bank 1 loaded → every slot starts in primary
  Press Prog 1 → LED green (primary)
  Press Prog 1 → LED blue (alt)        [slot 1 cache = alt]
  Press Prog 2 → LED green (primary)
  Press Prog 1 → LED BLUE (alt)        [restored from cache]
  Press Prog 1 → LED green (primary)
  ```
- **On bank switch** (commit), the alt cache is cleared, every slot starts in primary in the new bank.
- The cache is not persistent — **on boot, everything starts in primary**, regardless of `alt_toggle_behavior`.

If a slot has no alt configured, repeated press does nothing (no-op), the LED stays green.

### 2.6 Tap Tempo and Tuner

- **Tap tempo** (short press on the tap button): sends the globally configured `tap_message` on every press (typically CC, val 127). The tap LED blinks at the computed BPM (median of the last 3-4 presses).
- **Tuner toggle** (long press on the tap button, `long_press_short_ms`): sends the globally configured `tuner_message`. The state must be reflected visually (a separate LED or icon on the display).

Both messages are **global** settings, not mode/bank-level.

### 2.7 User Functions (Bank Up/Down long press)

- `user_function_a` and `user_function_b` are each a **MIDI message list** (may be null = no-op, or 1+ messages). Global setting. Editable from the web/encoder UI.

### 2.8 Expression pedal

- Analog signal read from ADC1_CH0, 12-bit (via the ESP-IDF `esp_adc` continuous or oneshot driver).
- Calibration: ADC values for heel/toe position (`adc_min`, `adc_max`) are stored. "Calibration" workflow on the web UI: press to heel → enter, press to toe → enter.
- Curve: `linear`, `log`, or `exp`. ADC range mapped to MIDI 0–127 along the curve.
- Smoothing: EMA filter, `smoothing` parameter (0–1, default 0.2). Output sends a new CC only when the computed MIDI value (0–127) has changed, at max ~100 Hz frequency.
- Target (CC# + channel):
  1. Slot-level `expression` override, if not null.
  2. Otherwise bank-level `expression_default`.
  3. Hierarchy: slot override > bank default. No global default.
- Expression behavior at the moment of alt toggle: no new message is sent on the old or new CC. On the next pedal movement, data starts flowing on the now-active target CC.

---

## 3. Configuration Mode: On-Device (Encoder)

### 3.1 General behavior

- From the main view, a short press on the encoder enters the menu.
- In the menu, encoder rotation navigates, short press enters / confirms, long press goes back / exits.
- **Footswitches work inside the menu too**: bank navigation, slot selection, MIDI sending behaves the same. The menu editor automatically switches to the new current slot if you change slots with your feet. This supports the audible-live editing workflow.

### 3.2 Menu hierarchy

```
[Main view]
└── encoder press
    └── [Menu]
        ├── Edit current preset
        │   ├── Name
        │   ├── Messages (list of PC/CC, add/edit/delete)
        │   ├── Alternative (create/remove, same editor)
        │   └── Expression target (CC + channel, or "inherit")
        ├── Edit current bank
        │   ├── Name
        │   └── Expression default (CC + channel)
        ├── Banks
        │   ├── Add new
        │   ├── Reorder
        │   └── Delete current
        ├── Global settings
        │   ├── MIDI channel
        │   ├── Tuner message
        │   ├── Tap message
        │   ├── User function A
        │   ├── User function B
        │   ├── Long press timings
        │   ├── Browse timeout
        │   ├── Alt toggle behavior (ALT_A / ALT_B)
        │   ├── Boot resume (on/off)
        │   ├── Display brightness
        │   └── Expression calibration
        ├── WiFi
        │   ├── SSID / password (input)
        │   ├── Show current IP / hostname
        │   └── Reset to AP mode
        └── Exit
```

### 3.3 Text input with the encoder

Character by character: encoder rotation = character change (A–Z, a–z, 0–9, space, `-_.`), encoder short press = next position, long press = save and exit. Backspace position: a `<` symbol at the end of the line; navigate there to delete.

### 3.4 Numeric input

Encoder rotation = value ±1 (with accelerated rotation detection = ±10 on fast movement). Short press = save.

---

## 4. Display Layouts

### 4.1 Main view (performance view)

Landscape orientation, 480 (w) × 320 (h). Must be readable from 160-170 cm distance.

Layout sketch:
```
┌───────────────────────────────────────────┐
│                                           │
│       <BANK NAME>                         │  ← ~50 px tall
│                                           │
│   ►  <PROGRAM NAME>  ◄                    │  ← ~90-100 px tall
│                                           │
│       [B<idx>/P<idx>]  ALT?  TUNER?       │  ← small indicator row
│                                           │
└───────────────────────────────────────────┘
```

- **Bank name**: max ~14 characters (font dependent).
- **Program name**: max ~10–12 characters (due to larger font).
- If alt is active: an `ALT` indicator in blue is shown below or next to the program name.
- If tuner is active: tuner indicator + tuner display (if the Nano Cortex provides SysEx feedback, optional — otherwise just ON/OFF).
- In browse mode: the top row shows the **target bank** name (marked with a `>` prefix to indicate preview).

### 4.2 Empty slot

If the selected slot is empty (no message, no name), the program name area shows a large `EMPTY` text. LED turns green, no MIDI is sent.

### 4.3 Menu view

Smaller font (~16–20 px), 6–10 menu items per page. Current item highlighted (background or inverted color). Hierarchical title at the top (e.g. `Global > Tuner message > CC number`).

### 4.4 Indicators

- WiFi status icon in the corner (connected / AP mode / offline).
- Hostname / IP in the menu.

---

## 5. LED Color Code (WS2812 chain)

| Slot state | Color |
|------------|-------|
| Inactive, normal | Red (low brightness, e.g. 30%) |
| Active, primary | Green (full brightness) |
| Active, alt | Blue (full brightness) |
| Browse mode, inactive | Red, blinking |
| Browse mode, active (current) | Its own state color (green or blue), blinking |

Tap LED (optional, the 6th WS2812): blinks at the computed BPM (~50% duty cycle).
Tuner LED (optional, the 7th WS2812): solid color while tuner is ON (e.g. yellow), dark while OFF.

Brightness is globally configurable (`led_brightness`, 0–255).

---

## 6. Configuration Schema

The full configuration is stored as JSON on persistent flash storage (LittleFS or equivalent). Example structure:

```yaml
global:
  midi_channel: 1
  tuner_message:        { type: CC, num: 68, val: 127, ch: 1 }
  tap_message:          { type: CC, num: 64, val: 127, ch: 1 }
  user_function_a:      null                   # bank↓ long press
  user_function_b:      null                   # bank↑ long press
  long_press_short_ms:  500
  long_press_long_ms:   1500
  browse_timeout_ms:    10000                  # 0 = disabled
  alt_toggle_behavior:  "ALT_B"                # "ALT_A" | "ALT_B"
  boot_resume:          true
  display_brightness:   200                    # 0–255
  led_brightness:       128                    # 0–255
  expression:
    adc_min:   200
    adc_max:   3900
    curve:     "linear"                        # "linear" | "log" | "exp"
    smoothing: 0.2

banks:
  - name: "Verse Setlist"
    expression_default: { cc: 11, ch: 1 }
    programs:
      - name: "Clean Intro"
        messages:
          - { type: PC, num: 4,  ch: 1 }
          - { type: CC, num: 20, val: 0, ch: 1 }
        expression: null                       # null = inherit bank default
        alternative:                           # optional, may be missing
          name: "Clean Intro ALT"
          messages:
            - { type: PC, num: 4,  ch: 1 }
            - { type: CC, num: 20, val: 127, ch: 1 }
          expression: null
      - name: ""                               # empty slot example
        messages: []
      # ... exactly 5 program slots per bank, empty ones allowed

  - name: "Chorus Setlist"
    # ...
```

### Constraints
- `PROGRAMS_PER_BANK = 5` (fixed, hardware-bound).
- `MAX_BANKS` is a soft limit, 32 is recommended, depending on flash capacity.
- MIDI message types: `PC` (Program Change, only `num`) and `CC` (Control Change, `num` + `val`). Every message has a `ch` field (1–16).
- Bank/program names and the alt name are optional. If empty, a placeholder is used (`Bank N`, `Slot M`, `EMPTY`).

---

## 7. Persistence and Boot Behavior

### 7.1 Stored data
- Full configuration (see schema) → LittleFS partition (`storage`), JSON format. (LittleFS is added via the IDF Component Manager — `joltwallet/littlefs`.)
- Last current bank / program index → NVS (`esp_nvs`) under the `state` namespace, only if `boot_resume = true`.
- WiFi credentials → NVS under the `wifi` namespace.
- Expression calibration → part of the `global.expression` block (lives in the LittleFS JSON, not NVS).

The partition table (`partitions.csv`) reserves `nvs` (24 K) for preferences, `phy_init` (4 K), an app partition (~4 MB), and a `storage` partition for LittleFS (filling the rest of the flash).

### 7.2 Non-persistent (RAM only)
- ALT_B cache (slot-level alt states).
- Target bank during browse.
- Tap tempo BPM.

### 7.3 Boot sequence
1. Initialize NVS (`nvs_flash_init`). On `ESP_ERR_NVS_NO_FREE_PAGES` / version mismatch, erase and re-init.
2. Mount the LittleFS `storage` partition. Load configuration JSON; if missing or corrupt, generate default config (1 empty bank, 5 empty slots) and write it back.
3. Initialize display, splash screen.
4. WiFi init via `esp_event` + `esp_netif`: if a saved SSID exists in NVS, start STA connection in the background (non-blocking).
5. If there's no SSID, or the STA connection doesn't succeed within N seconds, start AP mode with a captive portal.
6. mDNS registration (`mdns_init`, e.g. `midifoot.local`).
7. Start the web server (`esp_http_server`).
8. Load current bank/program: if `boot_resume`, the last state from NVS; otherwise `bank 0 / slot 0`. **Every slot starts in primary.**
9. Send the selected slot's MIDI messages (initial state sync with the Nano Cortex).
10. Show the main view.

---

## 8. WiFi and Web Configurator

### 8.1 Modes
- **STA mode**: connected to the user's WiFi network, accessible at `http://midifoot.local` (mDNS) or by IP.
- **AP mode**: own SSID (e.g. `MIDI-Footswitch-XXXX`), with a captive portal. On first boot and when STA mode times out.

### 8.2 Web UI features
- **Bank list** (reorder, add, delete).
- **Bank editor**: name, expression default, 5 program slots.
- **Slot editor**: name, MIDI message list (add/remove/edit), expression target, alt block (toggle on/off, same UI).
- **Global settings**: every field from the `global` block.
- **Expression calibration**: live ADC value display, "set as heel / set as toe" buttons.
- **WiFi setup**: SSID list (scan), password entry, IP info.
- **Live status panel** (via websocket, optional but strongly recommended): shows the currently active bank/slot, alt state, expression value, and last sent MIDI messages in real time. A lifesaver during debugging and learning.
- **Config export / import**: JSON file download / upload, so backup and setlist sharing are possible.

### 8.3 Other non-blocking aspects of configuration
- Web UI changes are only saved to flash when the user explicitly presses "Save" (during live editing they live in RAM).
- After save, the RAM configuration takes effect immediately, no reboot needed.

---

## 9. Module Boundaries (suggested software architecture)

| Module | Responsibility |
|--------|----------------|
| `InputManager` | Footswitch debounce, encoder read, long press detection, event push to a FreeRTOS queue |
| `StateMachine` | Current/target bank/program, browse mode, alt cache, tuner state. Only stores state and transitions |
| `MidiOut` | Send MIDI messages over UART1 (`uart_driver`), channel handling, throttling |
| `ExpressionPedal` | ADC1 read (`esp_adc`), smoothing, curving, MIDI send trigger |
| `DisplayDriver` | Low-level TFT driver over SPI2_HOST (`esp_lcd_panel_st7796` / `esp_lcd_panel_io_spi`), with DMA |
| `UiRenderer` | High-level UI (main view, menu), reading state from StateMachine. Optionally LVGL on top of the ESP-LCD panel handle |
| `MenuController` | Encoder menu logic, editors |
| `ConfigStore` | NVS (preferences, last state, WiFi creds) + LittleFS (JSON config blob), serialization/parsing, defaults |
| `WifiManager` | STA/AP mode handling (`esp_wifi` + `esp_netif`), captive portal, `mdns` |
| `WebServer` | `esp_http_server` endpoints, websocket (live status), static assets (embedded via `EMBED_FILES`) |
| `LedDriver` | WS2812 chain update via the RMT peripheral (`led_strip` component), animation state (blink, fade) |

`StateMachine` should be the single source of truth. `InputManager` sends events to it, `UiRenderer` and `LedDriver` read from it. `MidiOut` is triggered by `StateMachine` on state changes.

### Task / loop principle
- FreeRTOS tasks per module, communicating through queues and event groups. No task should block for more than a few ms outside of explicit waits.
- Time-critical: input debounce and MIDI out latency. These run on the higher-priority tasks and may be pinned to the PRO_CPU (core 0).
- Web server, mDNS, display refresh are lower priority and may be pinned to the APP_CPU (core 1) to keep them out of the input/MIDI hot path.

---

## 10. Open / Deferred questions

- **Enclosure**: size, material, manufacturing (bought vs 3D printed) – this affects the physical layout.
- **Tap LED and Tuner LED**: standalone WS2812s on the chain, or discrete LEDs? The chain length can be optimized.
- **Tuner display**: do we get feedback from the Nano Cortex via SysEx? If not, just an ON/OFF indicator.
- **Power management**: with a pedalboard PSU present, no sleep mode. Battery operation is not a goal right now.
- **OTA firmware update**: possible in the longer term, not MVP.
- **More expression inputs**: more ADC1 channels are free on GPIO 2–10 – if two pedals are ever needed, easy to extend. (ADC2 is not an option while WiFi is active.)
- **Long press on program footswitches**: not defined for now, but the logical framework allows future extension (e.g. slot scroll, function override).

---

## 11. MVP Order (suggested build order)

1. **Display + basic rendering**: TFT init, draw the main view with dummy data.
2. **Input + state machine**: footswitches, encoder, debounce, browse mode, alt toggle. Still no MIDI, state changes only visible on the display.
3. **MIDI out**: UART, message sending on state change.
4. **LED chain**: WS2812 control, color codes for the states.
5. **Expression pedal**: ADC, smoothing, calibration (hardcoded values for now), MIDI sending.
6. **Persistence**: JSON config to flash, load, save.
7. **WiFi + AP captive portal**: first-boot workflow.
8. **Web configurator**: REST endpoints, basic HTML/CSS/JS UI.
9. **Encoder menu**: on-device editor, covering every field.
10. **Live status websocket**: debug and demo-ability.
11. **Polish**: backlight PWM, LED brightness, font tweaks, empty slot rendering, error handling.

Each phase should be independently testable and should leave the firmware in a notionally deliverable state.
