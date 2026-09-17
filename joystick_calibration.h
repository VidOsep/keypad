#pragma once
//
//  Kalibracija joysticka za keypad - radialna normalizacija po rezinah.
//  Ohranja izvirno idejo (LUT maksimalnih radijev po kotih), popravlja pa
//  puscanje LUT, obstojnost, robne primere in mrtvo cono.
//
#include <Arduino.h>
#include <EEPROM.h>
#include "keypad_config.h"
#include <math.h>

// ------------------------------------------------------------------
//  Nastavitve
// ------------------------------------------------------------------
static const float    JS_MAX_VALUE   = 511.5f;   // sredina obsega ADC
static const int      JS_NUM_SLICES  = 36;       // 10 stopinj na rezino
static const float    JS_SLICE_ANGLE = TWO_PI / JS_NUM_SLICES;

static const float    JS_DECAY       = 0.90f;    // pozabljanje prevelikih sunkov, enkrat na prehod cez rezino
static const float    JS_RIM_GATE    = 0.50f;    // LUT se posodablja le nad tem delezem najvecjega radija
static const float    JS_AVG_ALPHA   = 0.01f;    // glajenje pri merjenju sredisca in smeri naprej
static const float    JS_MIN_FWD     = 30.0f;    // najmanjsi odklon (v enotah ADC) za veljavno smer naprej
static const float    JS_MIN_PEAK    = 32.0f;    // najmanjsi radij kroga za veljavno kalibracijo
static const float    JS_RIM_TRIM    = 0.98f;    // rahlo skrcen rob, da polni odklon zanesljivo doseze 100 %

static const uint16_t JS_DEBOUNCE_MS = 25;

static const uint16_t JS_CAL_MAGIC   = 0x4A53;   // 'JS'
static const uint16_t JS_CAL_VERSION = 2;
static const int      JS_EEPROM_ADDR = 0;

// Kotni popravek: brez njega je smer izhoda smer v SUROVEM (elipticnem)
// prostoru, kar diagonale zamakne. Pri razmerju osi 470/380 je napaka do 11
// stopinj. Vklopi z 1. Stane en atan2f, sinf in cosf na vzorec.
#ifndef JS_ANGLE_CORRECTION
#define JS_ANGLE_CORRECTION 0
#endif

// Zrcaljenje osi X in mrtva cona sta zdaj v EEPROM (glej keypad_config.h).
// Zrcaljenja kalibracija ne more ugotoviti sama: krog in izmerjena smer
// naprej dolocita preslikavo do zrcaljenja natancno, zato je to edini
// podatek, ki ga je treba povedati rocno. Popravek deluje na izhodu
// apply(), zato velja hkrati za gamepad osi in za tipkovnicne nacine.

// Vklopi za sporocila po serijski povezavi (v setup() rabis Serial.begin()).
#ifndef JS_DEBUG
#define JS_DEBUG 0
#endif
#if JS_DEBUG
  #define JS_LOG(s) Serial.println(F(s))
#else
  #define JS_LOG(s) do {} while (0)
#endif

enum CalibrationState { IDLE, GET_CENTER, GET_FORWARD, GET_CIRCLE };

// ------------------------------------------------------------------
//  Zapis kalibracije (locen od CRC, da je dolzina za CRC nedvoumna)
// ------------------------------------------------------------------
struct JSCalData {
    uint16_t magic;
    uint16_t version;
    float    centerX;
    float    centerY;
    float    angleRaw;                 // kot smeri naprej v surovem sistemu
    float    ellA;                     // manjsa polos prilagojene elipse (lezi pri ellTheta)
    float    ellB;                     // vecja polos
    float    ellTheta;                 // zasuk elipse
    float    lut[JS_NUM_SLICES];       // najvecji radij pri kotu i * JS_SLICE_ANGLE
};

struct JSCalRecord {
    JSCalData d;
    uint16_t  crc;
};

// ------------------------------------------------------------------
class JoystickCalibrator {
public:
    CalibrationState state = IDLE;
    JSCalData        cal;
    bool             lastResultOk = false;   // izid zadnje kalibracije (za prikaz z LED)

    void begin() {
        if (!loadFromEEPROM()) setDefaults();
    }

    // Programski zagon kalibracije (npr. iz akorda tipk).
    // lastBtn = true poskrbi, da tipka, ki je akord sprozila, ne premakne
    // takoj tudi prve stopnje - napredovanje rabi svez pritisk.
    void startCalibration() {
        state    = GET_CENTER;
        avgValid = false;
        lastBtn  = true;
        JS_LOG("KALIB: spusti palico, nato pritisni");
    }

    // --------------------------------------------------------------
    //  Klicati vsako iteracijo zanke s SUROVIMA vrednostma ADC.
    // --------------------------------------------------------------
    void update(int rawXi, int rawYi, bool calibButtonRaw) {
        const bool btn     = debounce(calibButtonRaw);
        const bool advance = btn && !lastBtn;
        lastBtn = btn;

        float rawX = (float)rawXi, rawY = (float)rawYi;
        transform(rawX);
        transform(rawY);

        switch (state) {

        case IDLE:
            if (advance) {
                avgX = rawX; avgY = rawY; avgValid = true;
                state = GET_CENTER;
                JS_LOG("KALIB: spusti palico, nato pritisni");
            }
            break;

        case GET_CENTER:
            trackAverage(rawX, rawY);
            if (advance) {
                cal.centerX = avgX;
                cal.centerY = avgY;
                avgValid = false;
                state = GET_FORWARD;
                JS_LOG("KALIB: drzi palico NAPREJ, nato pritisni");
            }
            break;

        case GET_FORWARD:
            trackAverage(rawX, rawY);
            if (advance) {
                const float fx = avgX - cal.centerX;
                const float fy = avgY - cal.centerY;
                if (calcDist(fx, fy) < JS_MIN_FWD) {
                    JS_LOG("KALIB: odklon premajhen, poskusi znova");
                    break;                       // ostani v tej stopnji
                }
                cal.angleRaw = calcAngle(fx, fy);
                avgValid  = false;
                rimMax    = 0.0f;
                lastSlice = -1;
                for (int i = 0; i < JS_NUM_SLICES; i++) cal.lut[i] = 0.0f;
                state = GET_CIRCLE;
                JS_LOG("KALIB: nekaj pocasnih krogov po robu, nato pritisni");
            }
            break;

        case GET_CIRCLE: {
            float x = rawX, y = rawY;
            normalize(x, y);
            const float dist = calcDist(x, y);
            if (dist > rimMax) rimMax = dist;

            // KLJUCNI POPRAVEK: brez tega pogoja sprosceni joystick med
            // cakanjem na zakljucni pritisk s puscanjem poje ze izmerjene rezine.
            if (dist >= JS_RIM_GATE * rimMax) {
                const float ang   = calcAngle(x, y);
                int         slice = (int)lroundf(ang / JS_SLICE_ANGLE);
                if (slice >= JS_NUM_SLICES) slice -= JS_NUM_SLICES;

                // Bledenje se uporabi ENKRAT ob vstopu v rezino, ne ob vsakem
                // vzorcu. Sicer je rezultat odvisen od tega, kako dolgo se v
                // kateri smeri zadrzujes in v katero smer vrtis - to zavrti
                // izmerjeno obliko za nekaj stopinj.
                if (slice != lastSlice) {
                    cal.lut[slice] *= JS_DECAY;
                    lastSlice = slice;
                }
                if (dist > cal.lut[slice]) cal.lut[slice] = dist;
            }

            if (advance) {
                if (finishLUT()) {
                    fitEllipse();
                    recomputeDerived();
                    saveToEEPROM();
                    lastResultOk = true;
                    JS_LOG("KALIB: koncano in shranjeno");
                } else {
                    lastResultOk = false;
                    JS_LOG("KALIB: premalo pokritja, kalibracija zavrnjena");
                    if (!loadFromEEPROM()) setDefaults();
                }
                state = IDLE;
            }
            break;
        }
        }
    }

    // --------------------------------------------------------------
    //  Surovi vrednosti -> kalibrirani osi 0..1023
    // --------------------------------------------------------------
    void apply(int rawXi, int rawYi, int &outX, int &outY) const {
        float x = (float)rawXi, y = (float)rawYi;
        transform(x);
        transform(y);
        normalize(x, y);

        const float ang  = calcAngle(x, y);
        const float dist = calcDist(x, y);

        const float f = ang / JS_SLICE_ANGLE;
        int   s = (int)f;
        float t = f - (float)s;
        if (s >= JS_NUM_SLICES) { s = JS_NUM_SLICES - 1; t = 1.0f; }
        const int slice = s;
        const int next  = (s + 1) % JS_NUM_SLICES;

        const float maxDist = (1.0f - t) * cal.lut[slice] + t * cal.lut[next];

        float r = (maxDist > 1.0f) ? (dist / maxDist) : 0.0f;
        if (r > 1.0f) r = 1.0f;                       // nikoli cez rob

        const float dz = kpDeadzone();
        float mag = 0.0f;
        if (r > dz)                                   // zvezna radialna mrtva cona
            mag = (r - dz) / (1.0f - dz);
        mag *= JS_MAX_VALUE;

        const float phi = correctAngle(ang) - angleOut + HALF_PI;
        outX = (int)lroundf(JS_MAX_VALUE + kpXSign() * mag * cosf(phi));
        outY = (int)lroundf(JS_MAX_VALUE + mag * sinf(phi));
    }

    void setDefaults() {
        cal.magic   = JS_CAL_MAGIC;
        cal.version = JS_CAL_VERSION;
        cal.centerX  = 0.0f;
        cal.centerY  = 0.0f;
        cal.angleRaw = 0.0f;
        cal.ellA = cal.ellB = 1.0f;      // enotska preslikava = brez popravka
        cal.ellTheta = 0.0f;
        for (int i = 0; i < JS_NUM_SLICES; i++) cal.lut[i] = JS_MAX_VALUE * 0.85f;
        recomputeDerived();
    }

    // Zaprtokrozna prilagoditev elipse na ze izmerjen LUT.
    // Ker so rezine enakomerno razporejene po kotu, so Fourierjevi
    // koeficienti kar resitev najmanjsih kvadratov za 1/r^2.
    void fitEllipse() {
        float P = 0.0f, Q = 0.0f, S = 0.0f;
        for (int i = 0; i < JS_NUM_SLICES; i++) {
            const float al  = i * JS_SLICE_ANGLE;
            const float inv = 1.0f / (cal.lut[i] * cal.lut[i]);
            P += inv;  Q += inv * cosf(2.0f * al);  S += inv * sinf(2.0f * al);
        }
        P /= JS_NUM_SLICES;  Q = 2.0f * Q / JS_NUM_SLICES;  S = 2.0f * S / JS_NUM_SLICES;
        const float K = sqrtf(Q*Q + S*S);
        if (!(P - K > 0.0f)) { cal.ellA = cal.ellB = 1.0f; cal.ellTheta = 0.0f; return; }
        cal.ellA     = 1.0f / sqrtf(P + K);
        cal.ellB     = 1.0f / sqrtf(P - K);
        cal.ellTheta = 0.5f * atan2f(S, Q);
    }

    void recomputeDerived() { angleOut = correctAngle(cal.angleRaw); }

    float correctAngle(float al) const {
#if JS_ANGLE_CORRECTION
        const float ap = al - cal.ellTheta;
        return cal.ellTheta + atan2f(cal.ellA * sinf(ap), cal.ellB * cosf(ap));
#else
        return al;
#endif
    }

private:
    // ---- pomozne pretvorbe (enake kot v izvirniku) ----
    static void transform(float &v)              { v -= JS_MAX_VALUE; }
    void normalize(float &x, float &y) const     { x -= cal.centerX; y -= cal.centerY; }

    static float calcAngle(float x, float y) {   // 0 = +Y (naprej), narasca v CCW
        float phi = -atan2f(x, y);
        if (phi < 0.0f) phi += TWO_PI;
        if (phi >= TWO_PI) phi = 0.0f;
        return phi;
    }
    static float calcDist(float x, float y)      { return sqrtf(x*x + y*y); }

    // ---- razbremenitev odboja tipke ----
    bool debounce(bool raw) {
        const uint32_t now = millis();
        if (raw != btnRaw) { btnRaw = raw; btnT = now; }
        if ((uint32_t)(now - btnT) >= JS_DEBOUNCE_MS) btnStable = btnRaw;
        return btnStable;
    }

    void trackAverage(float x, float y) {
        if (!avgValid) { avgX = x; avgY = y; avgValid = true; return; }
        avgX += (x - avgX) * JS_AVG_ALPHA;
        avgY += (y - avgY) * JS_AVG_ALPHA;
    }

    // ---- zakljucek: pociscenje in zapolnitev lukenj ----
    bool finishLUT() {
        float peak = 0.0f;
        for (int i = 0; i < JS_NUM_SLICES; i++)
            if (cal.lut[i] > peak) peak = cal.lut[i];
        if (!(peak > JS_MIN_PEAK)) return false;

        // Prava elipsa nikjer ne pade pod polovico najvecjega radija;
        // kar je nizje, je luknja ali smet iz zacetka zajema.
        const float floorVal = 0.5f * peak;
        int good = 0;
        for (int i = 0; i < JS_NUM_SLICES; i++) {
            if (cal.lut[i] < floorVal) cal.lut[i] = 0.0f;
            else                        good++;
        }
        if (good < (JS_NUM_SLICES * 2) / 3) return false;

        // Linearna interpolacija cez luknje, krozno.
        for (int i = 0; i < JS_NUM_SLICES; i++) {
            if (cal.lut[i] > 0.0f) continue;
            int lo = i, hi = i, dl = 0, dh = 0;
            do { lo = (lo + JS_NUM_SLICES - 1) % JS_NUM_SLICES; dl++; } while (cal.lut[lo] <= 0.0f);
            do { hi = (hi + 1) % JS_NUM_SLICES;                 dh++; } while (cal.lut[hi] <= 0.0f);
            const float t = (float)dl / (float)(dl + dh);
            cal.lut[i] = (1.0f - t) * cal.lut[lo] + t * cal.lut[hi];
        }

        // Zajem po predalih vzame maksimum cez celo rezino in zato rob rahlo
        // precenjuje; brez tega polnega odklona nikoli cisto ne dosezes.
        for (int i = 0; i < JS_NUM_SLICES; i++) cal.lut[i] *= JS_RIM_TRIM;
        return true;
    }

    // ---- obstojnost ----
    static uint16_t crc16(const uint8_t *p, uint16_t n) {
        uint16_t crc = 0xFFFF;
        while (n--) {
            crc ^= (uint16_t)(*p++) << 8;
            for (uint8_t i = 0; i < 8; i++)
                crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
        return crc;
    }

    bool loadFromEEPROM() {
        JSCalRecord rec;
        EEPROM.get(JS_EEPROM_ADDR, rec);
        if (rec.d.magic != JS_CAL_MAGIC || rec.d.version != JS_CAL_VERSION) return false;
        if (crc16((const uint8_t *)&rec.d, (uint16_t)sizeof(rec.d)) != rec.crc) return false;
        for (int i = 0; i < JS_NUM_SLICES; i++)
            if (!(rec.d.lut[i] > 1.0f)) return false;     // ujame tudi NaN
        cal = rec.d;
        recomputeDerived();
        return true;
    }

    void saveToEEPROM() {
        JSCalRecord rec;
        rec.d         = cal;
        rec.d.magic   = JS_CAL_MAGIC;
        rec.d.version = JS_CAL_VERSION;
        rec.crc       = crc16((const uint8_t *)&rec.d, (uint16_t)sizeof(rec.d));
        EEPROM.put(JS_EEPROM_ADDR, rec);                 // zapise samo spremenjene bajte
    }

    float    avgX = 0.0f, avgY = 0.0f;
    bool     avgValid = false;
    float    angleOut = 0.0f;
    float    rimMax = 0.0f;
    int      lastSlice = -1;
    bool     lastBtn = false, btnRaw = false, btnStable = false;
    uint32_t btnT = 0;
};
