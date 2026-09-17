# Keypad

A DIY gaming keypad in the spirit of an Azeron: 28 mechanical keys in a 5x6
matrix on printed finger and thumb parts, an analogue joystick, three status
LEDs, and an Arduino Pro Micro holding it together. The key map, the joystick
settings and the joystick calibration all live in EEPROM and are edited over
USB, so changing a binding never means recompiling.

Built by Vid, Miha and Andraz at FabLab Ljubljana.

## Repository layout

| file | what it is |
|---|---|
| `keypad.ino` | the sketch: matrix scan, debouncing, layers, joystick handling, serial protocol |
| `keypad_config.h` | settings and key map, their EEPROM record, defaults |
| `joystick_calibration.h` | centre / forward / radius-LUT calibration and the runtime mapping |
| `stick_to_wasd.h` | analogue stick to four direction keys, with hysteresis |
| `keypad_configurator.py` | the configurator (tkinter, no browser, no server) |
| `configurator.html` | the older browser configurator (WebSerial, needs a local server) |
| `docs/design-contract.md` | where the firmware is going: action model, EEPROM budget, protocol |

## Hardware

Arduino Pro Micro (ATmega32U4): 32 KB flash, 2.5 KB SRAM, 1 KB EEPROM.
28 keys (rows 0-3 have six columns, row 4 has four), joystick on A0/A1,
three LEDs. The matrix has a diode per key, so rows can be driven push-pull
and the keypad has full N-key rollover.

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

All six columns are on port B, so one `PINB` read samples a whole row at once.
LED0 and LED1 can be dimmed with `analogWrite`; LED2 has no hardware PWM.

## Building and uploading

Arduino IDE, board "Arduino Micro" (or Leonardo). The sketch needs the
[Joystick library](https://github.com/MHeironimus/ArduinoJoystickLibrary);
`Keyboard` and `EEPROM` ship with the AVR core. The folder must keep the name
`keypad`, since the IDE requires the sketch folder to match the `.ino`.

Holding both hexagon keys (5 and 11) for five seconds jumps straight to the
bootloader, which is easier than finding the reset pads.

## Using the configurator

```
pip3 install pyserial
python3 keypad_configurator.py
```

Pick the port (`/dev/cu.usbmodem*` on macOS), press Connect, and the keypad's
current configuration is read back. Pressing a physical key highlights it in
the window, so there is no guessing which index is which. Edits apply
immediately in RAM; **Write to EEPROM** makes them permanent.

On macOS the system `python3` links against Tk 8.5, which looks dated and is
buggy. Use the python.org build or `brew install python-tk`, and check with
`python3 -m tkinter`.

## Layers and the function chord

Four layers. Any key can be bound to "switch to layer N" in the configurator.
Keys 5 and 11 (the two hexagons) are also a chord — held together, the time
they are held decides what happens:

| held | result |
|---|---|
| under 1.5 s | next joystick mode |
| 2 s or more | start joystick calibration |
| 5 s or more | jump to the bootloader |

Each of the two still works as an ordinary key on its own.

## Joystick

Three modes: gamepad (HID axes), WASD, and arrow keys. Calibration measures the
centre, the forward direction, and the maximum radius in 36 angular slices, so
the stick reads full scale in every direction and the mounting angle does not
matter. The configurator plots the raw trail and marks where the stick comes to
rest after release, which is what the dead zone has to cover.

By default the keypad does **not** write the current layer and joystick mode to
EEPROM as they change, to avoid spending write cycles on them; there is a switch
in the configurator to turn that on. Write to EEPROM saves the current state
either way.

## Serial protocol

115200 baud over USB CDC, one command per line.

| command | meaning |
|---|---|
| `?` | identify: `KP <version> <keys> <layers>` |
| `G` | dump settings and key map |
| `K l i c m` | layer `l`, key `i` = code `c` with modifier mask `m` |
| `P f v` | set field `f` to `v` (`m` mode, `x` mirror, `d` dead zone, `o` `f` `a` `b` thresholds, `p` persist layer/mode) |
| `W` | write to EEPROM |
| `D` | load defaults into RAM |
| `L` | dump calibration and the radius LUT |
| `Y n` | switch to layer `n` |
| `M n` | switch joystick mode |
| `C` | start calibration |
| `T1` / `T0` | joystick and key telemetry on / off |
| `X1` / `X0` | mute key output while editing |

Key codes are the Arduino `Keyboard.h` values: printable characters are their
own ASCII code, `0x80`-`0x87` are the modifiers, `0xB0`+ the special keys, and
`0x88 + n` means "switch to layer n".
