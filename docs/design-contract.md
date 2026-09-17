# Keypad software — design contract (draft v2, 2026-09-16)

Status: **v2.1 — written after reading the actual firmware** (`keypad.ino`, `keypad_config.h`, `joystick_calibration.h`, `stick_to_wasd.h`) and `configurator.html`. Several things in v1 were guesses that the code has now corrected. Since then Vid has confirmed the board has per-key diodes, and debouncing has been implemented. Sections 1, 5 and 6 are the contract.

Language rule (team decision): all code, comments, identifiers and GUI text in **English**. The existing sources have Slovenian comments; they get translated as each file is touched, not in one big pass.

## 0. Hardware baseline

Arduino Pro Micro (ATmega32U4): 32 KB flash, 2.5 KB SRAM, **1 KB EEPROM**. 28 keys in a 5×6 matrix (row 4 has only 4 columns), joystick on A0/A1, 3 LEDs.

| function | Pro Micro label | port bit |
|---|---|---|
| row0 | D2 | D1 |
| row1 | D3 | D0 |
| row2 | D4 | D4 |
| row3 | D20/A2 | F5 |
| row4 | D21/A3 | F4 |
| col0 | D14 | B3 |
| col1 | D8/A8 | B4 |
| col2 | D10/A10 | B6 |
| col3 | D16 | B2 |
| col4 | D9/A9 | B5 |
| col5 | D15 | B1 |
| LED0 | D5 | C6 |
| LED1 | D6 | D7 |
| LED2 | D7 | E6 |

- **All six columns are on port B.** One `PINB` read per row samples the whole row at a single instant, instead of six separate reads.
- **LED0 (C6) and LED1 (D7) have hardware PWM** (OC3A/OC4A, OC4D). **LED2 (E6) has none** — software PWM, or a bigger series resistor.
- **D0/D1 are the I²C pins** and are matrix rows, so an external I²C EEPROM is not an option on the existing boards. 1 KB is the hard ceiling.

## 0a. Matrix scan review

The pin mapping in the new `readMatrix()` is **correct**: every row constant matches the table, and all five rows read the columns in the same order (B3, B4, B6, B2, B5, B1 = col0…col5). `matrixButtons` is already sized `KP_KEYS` = 28 and all 28 entries are written, so there is no stale-index problem — that concern from v1 was wrong.

**The board has per-key diodes** (confirmed), which settles the scan design: driving inactive rows HIGH is safe, `delayMicroseconds(10)` is plenty, and the matrix has full N-key rollover with no ghosting. Nothing in `readMatrix()` or `setup()` needs to change.

(For the record, had there been no diodes: two keys held in the same column on different rows would have shorted a pin driving HIGH to one driving LOW, the fix being high-Z inactive rows plus a longer settle time. Not applicable here.)

One thing still open:

4. **`ledResult()` blocks** with `delay(700)` and eight `delay(70)`s — up to 1.1 s during which the matrix is not scanned and HID is not serviced. It should become part of the LED state machine.

## 0a-2. Debouncing (implemented)

`pressKeys()` did pure edge detection on `matrixButtons[]` with nothing but the 1 ms loop delay behind it. `debounceMatrix()` now sits between `readMatrix()` and everything else:

- **Eager, per key.** A change is accepted on the first scan that sees it and the key is then locked for `KP_DEBOUNCE_MS` (5 ms) while the contact rattles. This adds **no latency** — unlike the common "accept only once the state has been stable for N ms", which delays every press by N ms. It matters on a gaming keypad.
- **Counts real milliseconds, not scan cycles**, so it does not care how long a loop iteration takes — including the 700 ms `ledResult()` stall.
- **Writes the clean state back into `matrixButtons[]`**, so `pressKeys()`, `handleChord()`, the calibration advance and the telemetry mask all get debounced data without knowing it exists.
- Costs 60 bytes of RAM (28 states + 28 timers + a timestamp).

A press shorter than 5 ms is reported as a 5 ms press rather than being dropped — the deliberate trade of the eager approach.

Verified with a unit-test harness compiling the block verbatim: zero added latency on a clean press, one edge out of 5 ms of rattle on both press and release, a real 15 ms double-tap kept intact, nothing stuck across a 700 ms stalled loop, correct behaviour across the `millis()` wrap, and keys independent of each other. Not yet compiled by the Arduino toolchain — that check is Vid's, on the next upload.

## 0b. The col2/col3 swap

col2 and col3 changed pins, so a physical key that used to report as index *r*·6+2 now reports as *r*·6+3 and vice versa.

- **Defaults:** swap the entries at index pairs (2,3), (8,9), (14,15), (20,21), (26,27) in `KP_DEFAULT_KEYS`.
- **Saved configs:** the other two keypads have keymaps already in EEPROM that will come out with two columns swapped. Bump `KP_VERSION` to 3 and, instead of discarding a version-2 record, migrate it: apply the same index swap to every layer of `keymap` and `mods`, then save. `kpLoad()` currently returns false on any version mismatch and silently falls back to defaults — that would wipe their mappings.

The two system keys are at indices 5 and 11 (column 5), which the swap does not touch.

## 0c. What the firmware already does — don't rebuild it

- The **function chord** (`n` + `b`, indices 11 and 5, held together): short press cycles joystick mode, 2 s starts calibration, **5 s jumps to the bootloader**. The bootloader jump is genuinely useful and must survive the redesign — suggest keeping it on both system keys held together.
- **Telemetry already streams the matrix as a 28-bit mask** (`t` line), so "press a key and it lights up in the GUI" works today.
- **`X1`/`X0` suppresses key output** while the GUI is editing, and releases itself if the serial port goes away.
- **Modifier reference counting** in `keyDown`/`keyUp` so two keys sharing Shift don't cancel each other. Keep this; it is easy to lose in a rewrite.
- **`mirrorX`**, because the calibration cannot determine handedness on its own.
- Calibration already does the **36-slice radius LUT, ellipse fit and angle correction** — the v1 proposal of 16 bins is a step backwards; keep 36.

## 1. Button action model

Today a key is **2 bytes** — `keymap[layer][i]` (an Arduino `Keyboard.h` code) and `mods[layer][i]` (a 4-bit modifier mask) — and "change layer" is encoded inside the keycode space as `0x88 + n`. That is compact but it has run out of room: there is nowhere to put hold-variants, autofire rates or joystick-mode actions.

Proposed replacement, **3 bytes per key per layer**:

```c
typedef struct {
  uint8_t type;   // ActionType
  uint8_t p1;
  uint8_t p2;
} KeyAction;      // 3 bytes
```

**Key codes stay in the Arduino `Keyboard.h` convention** — printable characters are their own ASCII value, 0x80–0x87 are the modifiers, 0xB0+ the specials — because `Keyboard.press()` takes them directly and the configurator already speaks it. No switch to raw HID usage IDs.

| type | p1 | p2 | behaviour |
|---|---|---|---|
| `ACT_NONE` | – | – | does nothing |
| `ACT_TRANS` | source | – | fall through. 0 = the layer's parent, N+1 = layer N specifically |
| `ACT_KEY` | keycode | modifiers | today's behaviour |
| `ACT_TOGGLE_KEY` | keycode | modifiers | press once → held; press again → released |
| `ACT_AUTOFIRE` | keycode | rate (Hz) | repeats while held |
| `ACT_TAP_HOLD` | keycode (tap) | layer (hold) | tap = key, hold = momentary layer |
| `ACT_LAYER_SET` | layer | – | today's `0x88 + n` |
| `ACT_LAYER_HOLD` | layer | – | layer N while held, back on release |
| `ACT_LAYER_ONESHOT` | layer | – | next key press comes from layer N |
| `ACT_JOY_MODE_SET` | mode | – | latch joystick mode |
| `ACT_JOY_MODE_HOLD` | mode | – | joystick mode while held |
| `ACT_JOY_MODE_TOGGLE` | – | – | cycle through all joystick modes |
| `ACT_LAYER_JOY_MODE_SET` | layer | mode | layer **and** joystick mode — one key = one game profile |
| `ACT_LAYER_JOY_MODE_HOLD` | layer | mode | both, while held |
| `ACT_JOY_BTN` | button | – | joystick-library button (`joystickButtonCount` is 0 today — raise it) |
| `ACT_MOUSE_BTN` | button | – | mouse button |
| `ACT_MEDIA` | consumer code | – | volume, play/pause |
| `ACT_MACRO` | macro index | – | optional, see the budget in section 5 |

**Joystick mode stays independent of layer.** The two `ACT_LAYER_JOY_MODE_*` actions exist for whoever wants them coupled.

**Layer inheritance.** Each layer gets a **parent layer** byte. `ACT_TRANS` resolves through the parent chain; `p1` is an optional per-key override, free because `ACT_TRANS` has no other parameter. Cap the resolution at 4 hops so a parent cycle cannot hang the firmware, and have the GUI refuse to create one.

Dispatch is a `switch` on `type`, driven by a per-key state machine with three events: **press**, **release**, **hold-timeout**. A new function = one enum value, one `case`, one row in the configurator's action table. The enum must live in exactly one place shared by firmware and configurator, or the two drift and configs corrupt silently.

**System keys (5 and 11).** The **tap** action stays fully configurable. The **hold** is hard-wired: key 5 → permanently set layer 0; key 11 → start calibration. Both held together for 5 s → bootloader.

**RAM.** Don't mirror the whole keymap. Cache the active layer (28 × 3 = 84 B) and reload it on a layer change.

## 2. Joystick modes

Today: `MODE_GAMEPAD`, `MODE_WASD`, `MODE_ARROWS`, where WASD and arrows are the same `StickToWasd` object with different key codes. That generalises cleanly into three *kinds*:

1. `JOY_ANALOG` — HID gamepad axes, as now.
2. `JOY_KEYS` — `StickToWasd` with **8 directions, each an arbitrary keycode + modifiers**, plus its existing hysteresis thresholds and a repeat rate. WASD, arrows and Onshape become presets, not firmware modes. (`StickToWasd` currently handles 4 directions and derives diagonals from overlap; going to 8 explicit directions is the main change.)
3. `JOY_MOUSE` — relative mouse movement: report on a fixed ~8 ms timer; velocity `v = vmax · ((r − dz)/(1 − dz))^gamma` with gamma ≈ 1.5–2.0; and a **float accumulator** per axis sending only the integer part and carrying the remainder, or slow movement rounds to zero and the pointer never creeps.

HID risk worth testing early: the sketch already registers `Keyboard` + `Joystick_`; adding `Mouse` makes it a three-interface composite on the 32U4. It should work through PluggableUSB, but test before building on it.

## 3. LED feedback

The firmware currently drives two LEDs and shows the joystick mode continuously, with a double blip when off the base layer. With LED0 back, the plan Vid asked for becomes possible: **light up only on a change**, with a code unique to the layer or mode.

- **Layer change:** the layer number in binary on the three LEDs for ~400 ms, then off.
- **Joystick mode change:** the same binary code pulsed twice, so a mode is never read as a layer.
- **Momentary layer hold:** the pattern stays lit while the key is held.
- **Error:** three fast blinks on all three.
- **Calibration:** one LED per step, as in section 4.

All of it a `millis()` state machine — which also means `ledResult()` loses its `delay()`s. Brightness: LED0 and LED1 can be dimmed with `analogWrite`, LED2 cannot; either software-PWM all three or raise the series resistors (330 Ω → 2.2–10 kΩ).

## 4. Joystick calibration

### 4.1 The centre is not a single point

A spring-return stick does not come back to the same place — where it settles depends on the direction it came from. Reading the centre once, at the start of calibration, is therefore wrong, and a dead zone derived from that reading's noise is far too small. Three mechanisms:

1. **Measure the resting spread instead of assuming it.** Record where the stick settles after each release during calibration rather than once at the start. Centre = mean of the resting points; dead zone = the box enclosing them plus a margin, **stored per direction (+x, −x, +y, −y)**, because stiction is not symmetric. Today `deadzone` is one number for both axes.
2. **Slow adaptive re-centring at runtime.** When the reading has been inside the dead zone and static for over ~1 s, nudge the centre estimate toward it with heavy smoothing (α ≈ 0.02), clamped so the centre can never leave the original dead zone. That bounds the drift and removes the need to recalibrate for wear.
3. **Manual override in the GUI.** The configurator now plots the raw trail and marks resting points in red, so the spread is visible and the dead zone can be set from evidence rather than guessed.

### 4.2 Sequence

Unchanged from the existing three-state machine (`GET_CENTER`, `GET_FORWARD`, `GET_CIRCLE`), which already works, plus:

- **One LED per step** now that there are three.
- **Forward detection without a button press:** wait until `r` exceeds ~60 % of possible travel *and* the last ~300 ms vary by less than a threshold — i.e. the stick is parked against its stop and held still. That is what makes the step automatic instead of requiring a press of `b`.
- **Record the resting point after each release** as the scatter samples for 4.1(1).
- Blink rate rising as the 3 s circle step runs out, as a countdown you can feel.
- Keep the existing 36-slice LUT, ellipse fit and angle correction.

## 5. EEPROM layout and budget

What is actually in EEPROM today:

```
0x000 .. 0x0AD   joystick calibration   174 B   JSCalRecord: header, 6 floats,
                                                36-float LUT, crc16
0x0AE .. 0x0FF   free                    82 B
0x100 .. 0x1ED   configuration          238 B   KPConfig + crc16:
                                                12 B settings + 112 B keymap
                                                + 112 B mods
0x1EE .. 0x3FF   free                   530 B
```

Going to 3 bytes per key costs 28 B per layer more than today. Budget for the new model, keeping the calibration block as it is:

| part | bytes |
|---|---|
| joystick calibration (36-float LUT) | 174 |
| config header + settings | 20 |
| joystick modes, 20 B each × 4 | 80 |
| **fixed total** | **274** |
| per layer (1 B parent + 28 × 3 B) | 85 |

750 B are left for layers, so **8 layers fit** (274 + 8 × 85 = 954). Eleven would need 1209 B.

**To get to 10 layers, pack the LUT.** It is 36 `float`s = 144 B storing a radius that never needs more than 8 bits of precision. As `uint8_t` fractions of the maximum radius it is 36 B, saving 108 B — the calibration block drops to ~66 B and the fixed total to 166 B, which leaves room for **10 layers**. Nothing else in the firmware has to change; only `loadFromEEPROM`/`saveToEEPROM` pack and unpack.

**Macros.** A step is keycode + modifiers + delay = 3 B, plus 1 B of length, so an 8-step macro is 25 B and **4 macros ≈ 100 B ≈ one layer**. They are affordable; make the layout **variable-length** (fixed header with a section length table, then the sections) and the choice between macros and layers becomes a per-user decision the configurator's budget bar can police, instead of a format decision made now.

**Layer and joystick mode are not auto-saved** (Andraz, 2026-09-17). Writing them to EEPROM on every change spends write cycles on state that is cheap to re-pick after a power cycle, so `persistState` gates the automatic write and is **off by default**; the configurator exposes it as a switch. An explicit *Write to EEPROM* stores whatever is current regardless. `startLayer` joins `startMode` in the settings block, and the format version went to 3.

Rules that already hold and should stay: `EEPROM.put()` writes only changed bytes; magic + version + CRC16 are checked on load. The one change is that a **version mismatch must migrate, not discard** (section 0b).

## 6. Serial protocol

**Extend the existing protocol, don't replace it.** It works, the firmware implements it, and both configurators speak it. Commands today: `?` identify, `G` dump config, `K l i c m` set key, `P f v` set field, `W` write EEPROM, `D` defaults, `L` read calibration + LUT, `Y n` set layer, `M n` set mode, `C` start calibration, `T1/T0` telemetry, `X1/X0` suppress keys. The `t` telemetry line already carries raw and calibrated axes, mode, layer, calibration state and the 28-bit key mask.

What the new model needs on top:

| cmd | meaning |
|---|---|
| `?` | extend the reply with **EEPROM size and bytes used**, so the GUI stops hardcoding 1024 |
| `K l i t p1 p2` | the existing set-key command gains a type byte — `K` with four arguments stays valid for `ACT_KEY` |
| `N n` | set the number of layers (the GUI refuses anything that would not fit) |
| `J m ...` | define joystick mode *m*: kind, thresholds, and the 8 direction codes |
| `A` | apply the RAM copy without writing EEPROM — already the implicit behaviour, worth naming |

## 7. Configurator (tkinter)

`keypad_configurator.py` is written and works against **today's** firmware — same protocol, same key codes, no server and no browser. It has the real physical layout (regular shapes, the thumb group separated by the gap), click-to-select, press-a-key-to-assign with modifiers, quick chips, layer-switch assignment, the joystick settings sliders, live key highlighting from the telemetry mask, and the raw/calibrated joystick views with the resting-point scatter from section 4.1.

- macOS: the system python3 links against Tk 8.5, which is ugly and buggy on recent macOS. Use the python.org build or `brew install python-tk`; check with `python3 -m tkinter`. Port is `/dev/cu.usbmodem*`.
- `pyserial` is the only dependency; the app opens and runs without it, just offline.
- Later, `pyinstaller` wraps it into a `.app` so Miha and Andraž need no Python.

It grows into the new action model as the firmware does: the action table is one list at the top of the file.

## 8. Open questions

1. Layer cap: 8 (as the EEPROM stands) or 10 (with the LUT packed)?
2. Do we keep the function chord as well as the two long-press actions, or does the chord disappear except for the 5 s bootloader jump?
3. Onshape preset: which shortcuts, exactly?
4. Per-user profiles, given that everyone puts space on a different finger?
5. `StickToWasd` goes from 4 keys to 8 directions — worth doing at the same time as the mouse mode, or separately?
