# WiFi MIDI Footswitch – Project Specification

Target device: Neural DSP **Nano Cortex**, controlled via MIDI from a footswitch interface, with on-device and web-based configuration.

---

## 1. Hardware

### 1.1 Components

| Component | Notes |
|-----------|-------|
| Raspberry Pi Pico W | Main MCU with built-in WiFi |
| 3.5" IPS TFT, ST7796, SPI, 480x320, no-touch | Main display |
| Rotary encoder (Bourns PEC11R or similar, integrated push button) | For on-device configuration |
| 8x SPST momentary footswitches | 2 bank + 5 program + 1 tap tempo |
| WS2812B / SK6812 LED chain (at least 5, optionally +2-3 status LEDs) | Program slot colors and status |
| Expression pedal input (TRS jack, typically 10k–25k log/lin pot) | Wired to ADC |
| MIDI TRS Type A output (instead of 5-pin DIN) | Feeds the Nano Cortex input |
| 9V DC power supply (from pedalboard PSU) | Buck converter to 5V for the Pico VBUS |

### 1.2 Suggested GPIO pinout (Pico W)

| GPIO | Function |
|------|----------|
| GP0 / GP1 | USB serial debug (reserved) |
| GP2 | Footswitch: Bank Down |
| GP3 | Footswitch: Bank Up |
| GP4 | UART1 TX → MIDI out (via 220Ω to TRS tip) |
| GP5 | Footswitch: Program 1 |
| GP6 | Encoder A |
| GP7 | Encoder B |
| GP8 | Encoder button |
| GP9 | WS2812 data |
| GP10 | Footswitch: Program 2 |
| GP11 | Footswitch: Program 3 |
| GP12 | Footswitch: Program 4 |
| GP13 | Footswitch: Program 5 |
| GP14 | Footswitch: Tap Tempo |
| GP15 | spare |
| GP16 | SPI0 MISO (TFT, optional) |
| GP17 | SPI0 CS (TFT) |
| GP18 | SPI0 SCK (TFT) |
| GP19 | SPI0 MOSI (TFT) |
| GP20 | TFT DC |
| GP21 | TFT RST |
| GP22 | TFT backlight (PWM) |
| GP26 | ADC0 → Expression pedal wiper |
| GP27 / GP28 | spare (additional ADC) |

GP23, GP24, GP25, GP29 are reserved for the Pico W's internal WiFi/LED.

### 1.3 MIDI output circuit

TRS Type A schematic (Nano Cortex input compatible):
- Tip → 220Ω → UART1 TX (GP4)
- Ring → 220Ω → 3.3V (or 5V VBUS if a more solid signal level is needed)
- Sleeve → GND

Note: the Nano Cortex TRS MIDI input is Type A standard (tip = current source). If the 3.3V signal isn't stable enough, a 74HCT04 inverter from 5V can level-shift it.

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

- Analog signal read from ADC0, 12-bit.
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
- Full configuration (see schema) → flash, JSON format.
- Last current bank / program index → flash (small separate file or preferences blob), only if `boot_resume = true`.
- WiFi credentials → flash.
- Expression calibration → part of the `global.expression` block.

### 7.2 Non-persistent (RAM only)
- ALT_B cache (slot-level alt states).
- Target bank during browse.
- Tap tempo BPM.

### 7.3 Boot sequence
1. Load configuration from flash. If missing or corrupt, generate default config (1 empty bank, 5 empty slots).
2. Initialize display, splash screen.
3. WiFi init (non-blocking): if a saved SSID exists, start connection in the background.
4. If there's no SSID, or the connection doesn't succeed within N seconds, start AP mode with a captive portal.
5. mDNS / Bonjour registration (e.g. `midifoot.local`).
6. Start the web server.
7. Load current bank/program: if `boot_resume`, the last state; otherwise `bank 0 / slot 0`. **Every slot starts in primary.**
8. Send the selected slot's MIDI messages (initial state sync with the Nano Cortex).
9. Show the main view.

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
| `InputManager` | Footswitch debounce, encoder read, long press detection, event push to event queue |
| `StateMachine` | Current/target bank/program, browse mode, alt cache, tuner state. Only stores state and transitions |
| `MidiOut` | Send MIDI messages over UART, channel handling, throttling |
| `ExpressionPedal` | ADC read, smoothing, curving, MIDI send trigger |
| `DisplayDriver` | Low-level TFT driver (SPI, DMA where possible) |
| `UiRenderer` | High-level UI (main view, menu), reading state from StateMachine |
| `MenuController` | Encoder menu logic, editors |
| `ConfigStore` | Flash persistence, JSON serialization/parsing, defaults |
| `WifiManager` | STA/AP mode handling, captive portal, mDNS |
| `WebServer` | HTTP endpoints, websocket (live status), static assets |
| `LedDriver` | WS2812 chain update (PIO-driven), animation state (blink, fade) |

`StateMachine` should be the single source of truth. `InputManager` sends events to it, `UiRenderer` and `LedDriver` read from it. `MidiOut` is triggered by `StateMachine` on state changes.

### Main loop principle
- Non-blocking, cooperative multitasking. No task should "freeze" for more than a few ms.
- Time-critical: input debounce and MIDI out latency. These should be the most frequently polled.
- Web server and display refresh are lower priority.

---

## 10. Open / Deferred questions

- **Enclosure**: size, material, manufacturing (bought vs 3D printed) – this affects the physical layout.
- **Tap LED and Tuner LED**: standalone WS2812s on the chain, or discrete LEDs? The chain length can be optimized.
- **Tuner display**: do we get feedback from the Nano Cortex via SysEx? If not, just an ON/OFF indicator.
- **Power management**: with a pedalboard PSU present, no sleep mode. Battery operation is not a goal right now.
- **OTA firmware update**: possible in the longer term, not MVP.
- **More expression inputs**: ADC1, ADC2 are free – if two pedals are ever needed, easy to extend.
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
