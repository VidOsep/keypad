//
//  Keypad - matrika 5x6, plasti, analogni joystick s kalibracijo
//
//  V mapi morajo biti se keypad_config.h, joystick_calibration.h in
//  stick_to_wasd.h, mapa pa se mora imenovati enako kot ta skec.
//
//  Nastavitve in keymap so v EEPROM in se urejajo prek GUI-ja po serijski
//  povezavi; ponovno nalaganje za spremembo tipk ni potrebno.
//
#define SET_PIN_MODE_OUTPUT(port, pin) DDR ## port |= (1 << pin)
#define SET_PIN_MODE_INPUT(port, pin)  DDR ## port &= ~(1 << pin)

#define SET_PIN_HIGH(port, pin) (PORT ## port |= (1 << pin))
#define SET_PIN_LOW(port, pin)  ((PORT ## port) &= ~(1 << (pin)))
#define PIN_TOGGLE(port, pin)   (PORT ## port) ^= (1 << (pin))

#define PIN_READ(port, pin) (PIN ## port & (1 << pin))

#include <Keyboard.h>
#include <Joystick.h>
#include <EEPROM.h>
#include <avr/wdt.h>
#include <stdlib.h>

// Kotni popravek elipse: 1 = vklopljen (priporoceno), 0 = izklopljen.
#define JS_ANGLE_CORRECTION 1

#include "keypad_config.h"          // mora biti pred naslednjima dvema
#include "joystick_calibration.h"
#include "stick_to_wasd.h"

// ------------------------------------------------------------------
//  Gamepad
// ------------------------------------------------------------------
const uint8_t joystickButtonCount = 0;
Joystick_ controller(JOYSTICK_DEFAULT_REPORT_ID, JOYSTICK_TYPE_GAMEPAD, joystickButtonCount,
                     0, true, true, false,
                     false, false, false,
                     false, false, false,
                     false, false);

const int AXIS_X_PIN = A0;
const int AXIS_Y_PIN = A1;

int lastXAxisValue = -1;
int lastYAxisValue = -1;

// ------------------------------------------------------------------
//  Matrika
// ------------------------------------------------------------------
const int rowCount    = 5;
const int colCount    = 6;
const int buttonCount = KP_KEYS;

bool    matrixButtons[buttonCount];
bool    keyHeld[buttonCount];                // fizicno drzana in ze obdelana
uint8_t heldCode[buttonCount];               // koda za spust, 0 = ni kaj spustiti
uint8_t heldMods[buttonCount];               // modifikatorji, drzani ob njej
uint8_t curLayer = 0;

// Modifikatorje stejemo, ne le pritiskamo in spuscamo. Ce dve tipki hkrati
// rabita Shift, ga spust prve ne sme odvzeti drugi - Arduinova Keyboard.h
// drzi modifikatorje kot masko in nima pojma, kdo jih se potrebuje.
uint8_t modCount[8];

inline bool isMod(uint8_t c) { return c >= 0x80 && c <= 0x87; }

void keyDown(uint8_t c) {
    if (isMod(c)) { if (modCount[c - 0x80]++ == 0) Keyboard.press(c); }
    else Keyboard.press(c);
}
void keyUp(uint8_t c) {
    if (isMod(c)) { uint8_t &n = modCount[c - 0x80]; if (n && --n == 0) Keyboard.release(c); }
    else Keyboard.release(c);
}
void modsDown(uint8_t m) { for (uint8_t b = 0; b < 4; b++) if (m & (1 << b)) keyDown(0x80 + b); }
void modsUp(uint8_t m)   { for (uint8_t b = 0; b < 4; b++) if (m & (1 << b)) keyUp(0x80 + b); }

// Spust tipke: najprej tipka, sele nato modifikatorji - v obratnem vrstnem
// redu bi gostitelj lahko videl golo tipko brez Shifta.
void releaseSlot(uint8_t i) {
    if (!heldCode[i]) return;
    keyUp(heldCode[i]);
    modsUp(heldMods[i]);
    heldCode[i] = 0;
    heldMods[i] = 0;
}

//MATRIX PINOUT
//syntax: "what is written on the pro micro" == port number
//
//row0 := "D2" == D1      col0 := "D14" == B3
//row1 := "D3" == D0      col1 := "D8/A8" == B4
//row2 := "D4" == D4      col2 := "D16" == B2
//row3 := "D20/A2" == F5  col3 := "D10/A10" == B6
//row4 := "D21/A3" == F4  col4 := "D5" == C6
//                        col5 := "D15" == B1

// ------------------------------------------------------------------
//  Funkcijski akord:  'n' (indeks 11) + 'b' (indeks 5)
//
//  Obe tipki delujeta normalno vsaka zase; funkcija se sprozi sele, ko
//  sta drzani skupaj, in je odvisna od tega, kako dolgo. Odloci se ob
//  SPUSTU, lucki pa med drzanjem povesta, kaj sledi.
//
//    kratko (do 1,5 s)   lucki ugasnjeni   -> naslednji nacin v krogu
//    2 s ali vec         LED1 sveti        -> zacne kalibracijo
//    5 s ali vec         obe svetita       -> skok v bootloader
//
//  Indeksa sta fizicna in neodvisna od plasti, da akord deluje vedno.
// ------------------------------------------------------------------
const int FN_BUTTON  = 11;
const int FN_PARTNER = 5;

const uint32_t CHORD_MODE_MAX = 1500;
const uint32_t CHORD_CALIB_MS = 2000;
const uint32_t CHORD_BOOT_MS  = 5000;

bool     chordActive = false;
uint32_t chordStart  = 0;

// Ko GUI ureja tipke, jih keypad ne sme posiljati v racunalnik.
bool     suppressKeys = false;

// ------------------------------------------------------------------
//  Kalibracijske lucke
//  LED0 odpade, ker je njen pin prevzel col4. Ostaneta LED1 in LED2.
// ------------------------------------------------------------------
const int  LED1_PIN        = 6;
const int  LED2_PIN        = 7;
const bool LED_ACTIVE_LOW  = false;

JoystickCalibrator js;
StickToWasd        wasd;
KeypadMode         mode = MODE_GAMEPAD;
CalibrationState   prevCalibState = IDLE;

void handleChord();
void ledBoth(bool on);
void serialPoll();

// ------------------------------------------------------------------
void ledWrite(int pin, bool on) {
    digitalWrite(pin, (on != LED_ACTIVE_LOW) ? HIGH : LOW);
}
void ledBoth(bool on) { ledWrite(LED1_PIN, on); ledWrite(LED2_PIN, on); }

void updateLeds() {
    const uint32_t t = millis();

    // Med drzanjem akorda lucki povesta, kaj se bo zgodilo ob spustu.
    if (chordActive) {
        const uint32_t held = t - chordStart;
        ledWrite(LED1_PIN, held >= CHORD_CALIB_MS);
        ledWrite(LED2_PIN, held >= CHORD_BOOT_MS);
        return;
    }

    switch (js.state) {
        case GET_CENTER:                                  // korak 1
            ledWrite(LED1_PIN, (t % 500) < 250);
            ledWrite(LED2_PIN, false);
            break;
        case GET_FORWARD:                                 // korak 2
            ledWrite(LED1_PIN, false);
            ledWrite(LED2_PIN, (t % 500) < 250);
            break;
        case GET_CIRCLE:                                  // korak 3
            ledBoth((t % 250) < 125);
            break;
        default: {                                        // IDLE
            // Lucki kazeta nacin. Ce nisi v osnovni plasti, obe vsake
            // poldrugo sekundo dvakrat kratko utripneta cez to.
            const uint32_t ph = t % 1500;
            const bool blip = (curLayer != 0) && (ph < 60 || (ph >= 140 && ph < 200));
            ledWrite(LED1_PIN, blip || mode == MODE_WASD);
            ledWrite(LED2_PIN, blip || mode == MODE_ARROWS);
            break;
        }
    }
}

void ledResult(bool ok) {
    if (ok) {
        ledBoth(true);  delay(700);  ledBoth(false);      // dolg utrip = shranjeno
    } else {
        for (int i = 0; i < 8; i++) {                     // hitro utripanje = zavrnjeno
            ledBoth(true);  delay(70);
            ledBoth(false); delay(70);
        }
    }
}

// ------------------------------------------------------------------
void readMatrix() {

    SET_PIN_LOW(D, 1);
    delayMicroseconds(10);
    matrixButtons[0] = !PIN_READ(B, 3);   matrixButtons[1] = !PIN_READ(B, 4);
    matrixButtons[2] = !PIN_READ(B, 2);   matrixButtons[3] = !PIN_READ(B, 6);
    matrixButtons[4] = !PIN_READ(C, 6);   matrixButtons[5] = !PIN_READ(B, 1);
    SET_PIN_HIGH(D, 1);

    SET_PIN_LOW(D, 0);
    delayMicroseconds(10);
    matrixButtons[6]  = !PIN_READ(B, 3);  matrixButtons[7]  = !PIN_READ(B, 4);
    matrixButtons[8]  = !PIN_READ(B, 2);  matrixButtons[9]  = !PIN_READ(B, 6);
    matrixButtons[10] = !PIN_READ(C, 6);  matrixButtons[11] = !PIN_READ(B, 1);
    SET_PIN_HIGH(D, 0);

    SET_PIN_LOW(D, 4);
    delayMicroseconds(10);
    matrixButtons[12] = !PIN_READ(B, 3);  matrixButtons[13] = !PIN_READ(B, 4);
    matrixButtons[14] = !PIN_READ(B, 2);  matrixButtons[15] = !PIN_READ(B, 6);
    matrixButtons[16] = !PIN_READ(C, 6);  matrixButtons[17] = !PIN_READ(B, 1);
    SET_PIN_HIGH(D, 4);

    SET_PIN_LOW(F, 5);
    delayMicroseconds(10);
    matrixButtons[18] = !PIN_READ(B, 3);  matrixButtons[19] = !PIN_READ(B, 4);
    matrixButtons[20] = !PIN_READ(B, 2);  matrixButtons[21] = !PIN_READ(B, 6);
    matrixButtons[22] = !PIN_READ(C, 6);  matrixButtons[23] = !PIN_READ(B, 1);
    SET_PIN_HIGH(F, 5);

    SET_PIN_LOW(F, 4);
    delayMicroseconds(10);
    matrixButtons[24] = !PIN_READ(B, 3);  matrixButtons[25] = !PIN_READ(B, 4);
    matrixButtons[26] = !PIN_READ(B, 2);  matrixButtons[27] = !PIN_READ(B, 6);
    SET_PIN_HIGH(F, 4);
}
// ------------------------------------------------------------------
//  Debouncing
//
//  Eager, per key: a change is accepted the moment it is seen and the key
//  is then locked for KP_DEBOUNCE_MS while the contact rattles. This adds
//  no latency at all - the press reaches the host on the first scan that
//  sees it - unlike the usual "accept only after the state has been stable
//  for N ms", which delays every press by N ms. Nothing real is swallowed:
//  a finger cannot press and release inside the lock-out.
//
//  The lock-out counts real milliseconds rather than scan cycles, so it
//  does not care how long a loop iteration happens to take.
//
//  Everything downstream reads matrixButtons[], so the debounced state is
//  written back into it and nothing else has to know this exists.
// ------------------------------------------------------------------
static const uint8_t KP_DEBOUNCE_MS = 5;

bool     debState[buttonCount];      // accepted state
uint8_t  debLock[buttonCount];       // ms left before this key may change again
uint32_t debLastMs = 0;

void debounceMatrix() {
    const uint32_t now     = millis();
    const uint32_t delta   = now - debLastMs;              // wrap-safe on uint32_t
    const uint8_t  elapsed = (delta > 255) ? 255 : (uint8_t)delta;
    debLastMs = now;

    for (int i = 0; i < buttonCount; i++) {
        if (debLock[i] > elapsed) {
            debLock[i] -= elapsed;                         // still rattling, ignore input
        } else {
            debLock[i] = 0;
            if (matrixButtons[i] != debState[i]) {         // a real edge
                debState[i] = matrixButtons[i];
                debLock[i]  = KP_DEBOUNCE_MS;
            }
        }
        matrixButtons[i] = debState[i];
    }
}

// ------------------------------------------------------------------
//  Plasti
// ------------------------------------------------------------------
void setLayer(uint8_t n) {
    if (n >= KP_LAYERS || n == curLayer) return;

    // Sprosti vse drzano s KODO, s katero je bilo pritisnjeno. Ce bi
    // spuscal po novi plasti, bi ostale tipke stare plasti pritisnjene.
    // keyHeld ostane, da nova plast ne sprozi tipke, dokler je ne spustis.
    for (int i = 0; i < buttonCount; i++) releaseSlot(i);
    curLayer = n;
    kpCfg.startLayer = n;
    kpPersistState();
}

void pressKeys() {
    // suppress: med urejanjem v GUI-ju tipke ne smejo iti v racunalnik
    const bool block = (js.state != IDLE) || suppressKeys;

    for (int i = 0; i < buttonCount; i++) {
        bool want = matrixButtons[i];
        if (block) want = false;
        if (chordActive && (i == FN_BUTTON || i == FN_PARTNER)) want = false;

        if (want && !keyHeld[i]) {
            keyHeld[i] = true;
            const uint8_t code = kpCfg.keymap[curLayer][i];
            if (code >= KP_LAYER_BASE && code < KP_LAYER_BASE + KP_LAYERS) {
                heldCode[i] = 0;
                setLayer(code - KP_LAYER_BASE);
            } else if (code != KP_NONE) {
                const uint8_t m = kpCfg.mods[curLayer][i];
                modsDown(m);                            // modifikatorji pred tipko
                keyDown(code);
                heldCode[i] = code;
                heldMods[i] = m;
            } else {
                heldCode[i] = 0;
            }
        } else if (!want && keyHeld[i]) {
            keyHeld[i] = false;
            releaseSlot(i);
        }
    }
}

// ------------------------------------------------------------------
void setAxes(int x, int y) {
    bool sendUpdate = false;
    if (x != lastXAxisValue) { controller.setXAxis(x); lastXAxisValue = x; sendUpdate = true; }
    if (y != lastYAxisValue) { controller.setYAxis(y); lastYAxisValue = y; sendUpdate = true; }
    if (sendUpdate) controller.sendState();
}

// Isti mehanizem, ki ga uporablja Arduinov CDC, samo sprozen rocno.
void rebootToBootloader() {
    Keyboard.releaseAll();
    wasd.releaseAll();
    ledBoth(false);
    delay(20);
    USBDevice.detach();
    delay(20);
    *(uint16_t *)0x0800 = 0x7777;      // magicni kljuc za Caterina bootloader
    wdt_enable(WDTO_120MS);
    while (1) {}
}

void setMode(KeypadMode m) {
    if (m == mode) return;
    wasd.releaseAll();               // sprosti tipke STAREGA nacina, preden jih zamenjas
    mode = m;
    if (m == MODE_WASD)   wasd.setKeys('w', 'a', 's', 'd');
    if (m == MODE_ARROWS) wasd.setKeys(KEY_UP_ARROW, KEY_LEFT_ARROW, KEY_DOWN_ARROW, KEY_RIGHT_ARROW);
    setAxes(512, 512);               // da os ne obvisi odklonjena
    kpCfg.startMode = (uint8_t)mode;
}

KeypadMode nextMode(KeypadMode m) {
    if (m == MODE_GAMEPAD) return MODE_WASD;
    if (m == MODE_WASD)    return MODE_ARROWS;
    return MODE_GAMEPAD;
}

void handleChord() {
    if (js.state != IDLE) { chordActive = false; return; }

    const bool     chord = matrixButtons[FN_BUTTON] && matrixButtons[FN_PARTNER];
    const uint32_t now   = millis();

    if (chord && !chordActive) { chordActive = true; chordStart = now; return; }
    if (chord || !chordActive) return;

    chordActive = false;
    const uint32_t held = now - chordStart;

    if      (held >= CHORD_BOOT_MS)  rebootToBootloader();
    else if (held >= CHORD_CALIB_MS) js.startCalibration();
    else if (held <= CHORD_MODE_MAX) { setMode(nextMode(mode)); kpPersistState(); }
    // vmesno obmocje je namenoma brez ucinka
}

void joystickHandle() {
    const int rawX = analogRead(AXIS_X_PIN);
    const int rawY = analogRead(AXIS_Y_PIN);

    // Med kalibracijo stopnje premika 'b' sama; zagon gre prek akorda.
    js.update(rawX, rawY, (js.state == IDLE) ? false : matrixButtons[FN_PARTNER]);

    if (prevCalibState == GET_CIRCLE && js.state == IDLE) ledResult(js.lastResultOk);
    prevCalibState = js.state;

    if (js.state != IDLE) {          // med kalibracijo nic ne posiljaj
        wasd.releaseAll();
        setAxes(512, 512);
        return;
    }

    handleChord();

    int outX, outY;
    js.apply(rawX, rawY, outX, outY);

    if (mode == MODE_GAMEPAD) {
        setAxes(outX, outY);
    } else {
        wasd.update((outX - JS_MAX_VALUE) / JS_MAX_VALUE,
                    (outY - JS_MAX_VALUE) / JS_MAX_VALUE);
        setAxes(512, 512);
    }
}

// ------------------------------------------------------------------
//  Protokol po serijski - z njim govori GUI
//
//    ?          predstavitev
//    G          izpis celotne nastavitve
//    K l i c m  keymap[plast][indeks] = koda, m = maska modifikatorjev
//    P f v      nastavitev f na vrednost v (f = p: shranjuj plast in nacin)
//    W          zapis v EEPROM
//    D          privzete vrednosti (samo v RAM, dokler ni W)
//    L          izpis kalibracije in LUT
//    Y n        preklop na plast n
//    M n        preklop nacina
//    C          zagon kalibracije
//    T1 / T0    tok zivih podatkov palice
//    X1 / X0    zadusi tipke (med urejanjem, da ne tipkajo v GUI)
// ------------------------------------------------------------------
char     serBuf[40];
uint8_t  serLen      = 0;
bool     telemetry   = false;
uint32_t telemT      = 0;

void printHex2(uint8_t v) {
    static const char h[] = "0123456789ABCDEF";
    Serial.write((uint8_t)h[v >> 4]);
    Serial.write((uint8_t)h[v & 15]);
}

void serialLine(char *s) {
    char *p = s + 1;

    switch (s[0]) {

    case '?':
        Serial.print(F("KP "));  Serial.print(KP_VERSION);
        Serial.print(' ');       Serial.print(KP_KEYS);
        Serial.print(' ');       Serial.println(KP_LAYERS);
        break;

    case 'G':
        Serial.print(F("CFG "));
        Serial.print(kpCfg.startMode); Serial.print(' ');
        Serial.print(kpCfg.mirrorX);   Serial.print(' ');
        Serial.print(kpCfg.deadzone);  Serial.print(' ');
        Serial.print(kpCfg.wasdROn);   Serial.print(' ');
        Serial.print(kpCfg.wasdROff);  Serial.print(' ');
        Serial.print(kpCfg.wasdAOn);   Serial.print(' ');
        Serial.print(kpCfg.wasdAOff);  Serial.print(' ');
        Serial.println(kpCfg.persistState);
        for (uint8_t L = 0; L < KP_LAYERS; L++) {
            Serial.print(F("KM ")); Serial.print(L); Serial.print(' ');
            for (uint8_t i = 0; i < KP_KEYS; i++) printHex2(kpCfg.keymap[L][i]);
            Serial.println();
            Serial.print(F("MD ")); Serial.print(L); Serial.print(' ');
            for (uint8_t i = 0; i < KP_KEYS; i++) printHex2(kpCfg.mods[L][i]);
            Serial.println();
        }
        Serial.println(F("OK"));
        break;

    case 'K': {
        const long L = strtol(p, &p, 10);
        const long i = strtol(p, &p, 10);
        const long c = strtol(p, &p, 10);
        const long m = strtol(p, &p, 10);          // manjkajoc = 0
        if (L >= 0 && L < KP_LAYERS && i >= 0 && i < KP_KEYS && c >= 0 && c < 256
            && m >= 0 && m < 16) {
            kpCfg.keymap[L][i] = (uint8_t)c;
            kpCfg.mods[L][i]   = (uint8_t)m;
            Serial.println(F("OK"));
        } else Serial.println(F("ERR"));
        break;
    }

    case 'P': {
        while (*p == ' ') p++;
        const char f = *p++;
        long v = strtol(p, &p, 10);
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        switch (f) {
            case 'm': setMode((KeypadMode)(v > 2 ? 2 : v)); break;
            case 'x': kpCfg.mirrorX  = v ? 1 : 0;                break;
            case 'd': kpCfg.deadzone = (uint8_t)(v > 40 ? 40 : v); break;
            case 'o': kpCfg.wasdROn  = (uint8_t)v;               break;
            case 'f': kpCfg.wasdROff = (uint8_t)v;               break;
            case 'a': kpCfg.wasdAOn  = (uint8_t)v;               break;
            case 'b': kpCfg.wasdAOff = (uint8_t)v;               break;
            case 'p': kpCfg.persistState = v ? 1 : 0;            break;
            default:  Serial.println(F("ERR")); return;
        }
        Serial.println(F("OK"));
        break;
    }

    case 'W': kpSave();     Serial.println(F("OK")); break;
    case 'D': kpDefaults(); Serial.println(F("OK")); break;

    case 'L': {
        Serial.print(F("CAL "));
        Serial.print(js.cal.centerX + JS_MAX_VALUE, 1); Serial.print(' ');
        Serial.print(js.cal.centerY + JS_MAX_VALUE, 1); Serial.print(' ');
        Serial.print(js.cal.angleRaw, 4);               Serial.print(' ');
        Serial.print(js.cal.ellA, 2);                   Serial.print(' ');
        Serial.print(js.cal.ellB, 2);                   Serial.print(' ');
        Serial.println(js.cal.ellTheta, 4);
        Serial.print(F("LUT"));
        for (int i = 0; i < JS_NUM_SLICES; i++) { Serial.print(' '); Serial.print(js.cal.lut[i], 1); }
        Serial.println();
        break;
    }

    case 'Y': { const long n = strtol(p, &p, 10);
                if (n >= 0 && n < KP_LAYERS) { setLayer((uint8_t)n); Serial.println(F("OK")); }
                else Serial.println(F("ERR"));
                break; }

    case 'M': { const long n = strtol(p, &p, 10);
                if (n >= 0 && n <= 2) { setMode((KeypadMode)n); Serial.println(F("OK")); }
                else Serial.println(F("ERR"));
                break; }

    case 'C': js.startCalibration(); Serial.println(F("OK")); break;

    case 'T': telemetry   = (*p == '1'); Serial.println(F("OK")); break;
    case 'X': suppressKeys = (*p == '1'); Serial.println(F("OK")); break;

    default: Serial.println(F("ERR")); break;
    }
}

void serialPoll() {
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (serLen) { serBuf[serLen] = 0; serialLine(serBuf); serLen = 0; }
        } else if (serLen < sizeof(serBuf) - 1) {
            serBuf[serLen++] = c;
        }
    }

    // Ce GUI zapre vrata ali crkne, se dusenje tipk samo sprosti -
    // sicer bi ti keypad ostal mrtev do ponovnega priklopa.
    if (!Serial) { telemetry = false; suppressKeys = false; return; }

    if (telemetry && (uint32_t)(millis() - telemT) >= 20) {   // 50 Hz
        telemT = millis();
        const int rx = analogRead(AXIS_X_PIN);
        const int ry = analogRead(AXIS_Y_PIN);
        int ox, oy;
        js.apply(rx, ry, ox, oy);
        Serial.print(F("t "));
        Serial.print(rx); Serial.print(' ');
        Serial.print(ry); Serial.print(' ');
        Serial.print(ox); Serial.print(' ');
        Serial.print(oy); Serial.print(' ');
        Serial.print((int)mode);     Serial.print(' ');
        Serial.print((int)curLayer); Serial.print(' ');
        Serial.print((int)js.state); Serial.print(' ');
        // stanje matrike kot 28-bitna maska, da GUI ve, katero tipko drzis
        uint32_t mask = 0;
        for (int i = 0; i < buttonCount; i++) if (matrixButtons[i]) mask |= (1UL << i);
        Serial.println((unsigned long)mask, HEX);
    }
}

// ------------------------------------------------------------------
void setup() {
    Serial.begin(115200);                    // za GUI; 1200 bi sprozil bootloader

    // Kalibracija vrne 0..1023 s srediscem pri 512 in +Y naprej.
    // Ce se ti katera os zdi obrnjena, zamenjaj mejni vrednosti v tisti vrstici.
    controller.setXAxisRange(0, 1023);
    controller.setYAxisRange(1023, 0);       // HID ima +Y navzdol, mi racunamo naprej = +Y
    controller.begin(false);

    for (int i = 0; i < buttonCount; i++) { heldCode[i] = 0; heldMods[i] = 0; keyHeld[i] = false; }
    for (int i = 0; i < buttonCount; i++) { debState[i] = false; debLock[i] = 0; }
    debLastMs = millis();
    for (int i = 0; i < 8; i++) modCount[i] = 0;

    pinMode(LED1_PIN, OUTPUT);
    pinMode(LED2_PIN, OUTPUT);
    ledBoth(false);

    kpBegin();                               // nastavitve in keymap iz EEPROM
    js.begin();                              // kalibracija iz EEPROM

    mode = MODE_GAMEPAD;                     // izhodisce, da setMode res odpre nov nacin
    setMode(kpCfg.startMode <= 2 ? (KeypadMode)kpCfg.startMode : MODE_GAMEPAD);
    if (kpCfg.startLayer < KP_LAYERS) curLayer = kpCfg.startLayer;

    //initiate rows
    SET_PIN_MODE_OUTPUT(D, 1);  SET_PIN_HIGH(D, 1);
    SET_PIN_MODE_OUTPUT(D, 0);  SET_PIN_HIGH(D, 0);
    SET_PIN_MODE_OUTPUT(D, 4);  SET_PIN_HIGH(D, 4);
    SET_PIN_MODE_OUTPUT(F, 5);  SET_PIN_HIGH(F, 5);
    SET_PIN_MODE_OUTPUT(F, 4);  SET_PIN_HIGH(F, 4);

    //initiate columns
    SET_PIN_MODE_INPUT(B, 3);   SET_PIN_HIGH(B, 3);
    SET_PIN_MODE_INPUT(B, 4);   SET_PIN_HIGH(B, 4);
    SET_PIN_MODE_INPUT(B, 2);   SET_PIN_HIGH(B, 2);
    SET_PIN_MODE_INPUT(B, 6);   SET_PIN_HIGH(B, 6);
    SET_PIN_MODE_INPUT(C, 6);   SET_PIN_HIGH(C, 6);
    SET_PIN_MODE_INPUT(B, 1);   SET_PIN_HIGH(B, 1);
}

void loop() {
    readMatrix();
    debounceMatrix();
    joystickHandle();
    pressKeys();
    updateLeds();
    serialPoll();
    delay(1);
}
