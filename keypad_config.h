#pragma once
//
//  Nastavitve keypada: plasti, keymap in parametri palice.
//  Vse je v EEPROM in se da spreminjati po serijski, brez ponovnega nalaganja.
//
//  VKLJUCI TO DATOTEKO PRED joystick_calibration.h in stick_to_wasd.h -
//  obe bereta nastavitve od tu.
//
#include <Arduino.h>
#include <EEPROM.h>
#include <Keyboard.h>   // za KEY_* v privzetem keymapu

#define KP_LAYERS 4
#define KP_KEYS   28

// Nacini palice. Enum je namenoma TU in ne v .ino: Arduino IDE vstavi
// samodejne prototipe funkcij takoj za zadnjo #include vrstico, tako da
// bi prototip za setMode(KeypadMode) prisel pred definicijo enuma.
enum KeypadMode : uint8_t { MODE_GAMEPAD = 0, MODE_WASD = 1, MODE_ARROWS = 2 };

// Kode v tem obsegu niso tipke, ampak ukazi. 0x88-0x8B je v Arduinovi
// Keyboard.h prazen prostor med modifikatorji (0x80-0x87) in KEY_RETURN
// (0xB0). Obsega 0xF0-0xFB namenoma NE uporabljam - tam so v novejsih
// razlicicah jedra F13 do F24.
static const uint8_t KP_LAYER_BASE = 0x88;    // 0x88 + n = preklopi na plast n
static const uint8_t KP_NONE       = 0x00;    // prazno mesto, nic ne poslje

// Maska modifikatorjev, ki se drzijo skupaj s tipko (npr. Shift+E za izriv).
// Bit ustreza kodi 0x80 + bit v Arduinovi Keyboard.h.
static const uint8_t KP_MOD_CTRL  = 1 << 0;   // 0x80
static const uint8_t KP_MOD_SHIFT = 1 << 1;   // 0x81
static const uint8_t KP_MOD_ALT   = 1 << 2;   // 0x82
static const uint8_t KP_MOD_GUI   = 1 << 3;   // 0x83

static const uint16_t KP_MAGIC   = 0x4B50;    // 'KP'
static const uint16_t KP_VERSION = 3;
static const int      KP_EEPROM_ADDR = 256;   // kalibracija zaseda 0..175

struct KPConfig {
    uint16_t magic;
    uint16_t version;
    uint8_t  startMode;      // 0 gamepad, 1 WASD, 2 puscice
    uint8_t  mirrorX;        // 1 = zamenjaj levo in desno
    uint8_t  deadzone;       // odstotki polnega odklona
    uint8_t  wasdROn;        // odstotki: zacetek hoje
    uint8_t  wasdROff;       // odstotki: konec hoje
    uint8_t  wasdAOn;        // odstotki: prag komponente smeri
    uint8_t  wasdAOff;       // odstotki: spust tipke
    uint8_t  startLayer;     // layer restored at power-up
    uint8_t  persistState;   // 1 = write layer/mode to EEPROM whenever they change
    uint8_t  keymap[KP_LAYERS][KP_KEYS];
    uint8_t  mods[KP_LAYERS][KP_KEYS];     // modifikatorji ob tipki
};

struct KPRecord { KPConfig c; uint16_t crc; };

KPConfig kpCfg;

// Privzeti keymap: Vidova izvirna razporeditev. Vse plasti se zacnejo enake,
// da nobena plast ni slepa ulica, iz katere se ne da nazaj.
static const uint8_t KP_DEFAULT_KEYS[KP_KEYS] PROGMEM = {
    '0', 'x', 'v', 'c', 't', 'b', KEY_LEFT_ALT, KEY_LEFT_CTRL, ' ', KEY_LEFT_SHIFT,
    '\n', 'n', 'g', 'q', 'f', 'e', KEY_TAB, 'r', '1', '2',
    '4', '3', KEY_ESC, '9', '5', '6', '8', '7'
};

// --- dostop do nastavitev v obliki, ki jo rabita druga dva headerja ---
inline float kpDeadzone() { return kpCfg.deadzone * 0.01f; }
inline float kpXSign()    { return kpCfg.mirrorX ? -1.0f : 1.0f; }
inline float kpROn()      { return kpCfg.wasdROn  * 0.01f; }
inline float kpROff()     { return kpCfg.wasdROff * 0.01f; }
inline float kpAOn()      { return kpCfg.wasdAOn  * 0.01f; }
inline float kpAOff()     { return kpCfg.wasdAOff * 0.01f; }

static uint16_t kpCrc16(const uint8_t *p, uint16_t n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (uint8_t i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

void kpDefaults() {
    kpCfg.magic     = KP_MAGIC;
    kpCfg.version   = KP_VERSION;
    kpCfg.startMode = 0;
    kpCfg.mirrorX   = 1;        // Vidova plosca ima os X obrnjeno
    kpCfg.deadzone  = 6;
    kpCfg.wasdROn   = 50;
    kpCfg.wasdROff  = 32;
    kpCfg.wasdAOn   = 40;
    kpCfg.wasdAOff  = 26;
    kpCfg.startLayer   = 0;
    kpCfg.persistState = 0;   // off by default: do not spend EEPROM cycles on it
    for (uint8_t L = 0; L < KP_LAYERS; L++)
        for (uint8_t i = 0; i < KP_KEYS; i++) {
            kpCfg.keymap[L][i] = pgm_read_byte(&KP_DEFAULT_KEYS[i]);
            kpCfg.mods[L][i]   = 0;
        }
}

// Zavrne poskodovan ali star zapis in se vrne na privzete vrednosti.
bool kpLoad() {
    KPRecord rec;
    EEPROM.get(KP_EEPROM_ADDR, rec);
    if (rec.c.magic != KP_MAGIC || rec.c.version != KP_VERSION) return false;
    if (kpCrc16((const uint8_t *)&rec.c, (uint16_t)sizeof(rec.c)) != rec.crc) return false;
    if (rec.c.startMode > 2) return false;
    kpCfg = rec.c;
    return true;
}

void kpSave() {
    KPRecord rec;
    rec.c         = kpCfg;
    rec.c.magic   = KP_MAGIC;
    rec.c.version = KP_VERSION;
    rec.crc       = kpCrc16((const uint8_t *)&rec.c, (uint16_t)sizeof(rec.c));
    EEPROM.put(KP_EEPROM_ADDR, rec);      // pise samo spremenjene bajte
}

void kpBegin() { if (!kpLoad()) kpDefaults(); }

// Layer and joystick mode change often; writing them to EEPROM every time
// would burn through its ~100k cycles for no good reason. So the automatic
// write happens only when the user asks for it in the configurator. An
// explicit "write to EEPROM" still stores whatever is current either way.
void kpPersistState() { if (kpCfg.persistState) kpSave(); }
