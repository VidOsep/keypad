#pragma once
//
//  Analogni joystick -> stiri smerne tipke (WASD ali puscice).
//
//  Java nima analognega gibanja: tipka je pritisnjena ali ne. Od palice
//  torej ostane le izbira ene od osmih smeri, in edina stvar, ki jo je
//  treba dobro narediti, je da se ta izbira ne trese na mejah. Zato je
//  histereza povsod - locena za "ali sploh hodim" in za vsako tipko.
//
#include <Arduino.h>
#include <Keyboard.h>
#include "keypad_config.h"

// Pragovi so zdaj v EEPROM (glej keypad_config.h): kpROn, kpROff, kpAOn, kpAOff.

// Neobvezno: polni odklon vklopi tudi sprint. Ce imas Ctrl na tipki, pusti 0.
#ifndef WASD_SPRINT_ON_FULL
#define WASD_SPRINT_ON_FULL 0
#endif
static const float WASD_SPR_ON  = 0.90f;
static const float WASD_SPR_OFF = 0.72f;

class StickToWasd {
public:
    // Katere stiri tipke naj palica tipka. Poklici sele PO releaseAll(),
    // sicer stare tipke ostanejo pritisnjene v gostitelju.
    void setKeys(uint8_t up, uint8_t left, uint8_t down, uint8_t right) {
        kUp = up; kLeft = left; kDown = down; kRight = right;
    }

    // ux, uy v obsegu -1..1, +Y je naprej
    void update(float ux, float uy) {
        const float r = sqrtf(ux*ux + uy*uy);
        walking = walking ? (r > kpROff()) : (r > kpROn());

        bool nw = false, na = false, ns = false, nd = false, nspr = false;
        if (walking) {
            const float inv = 1.0f / (r > 1e-6f ? r : 1e-6f);
            const float dx = ux * inv, dy = uy * inv;      // smer, neodvisna od jakosti
            nw = hyst(kw,  dy);
            ns = hyst(ks, -dy);
            nd = hyst(kd,  dx);
            na = hyst(ka, -dx);
#if WASD_SPRINT_ON_FULL
            nspr = kspr ? (r > WASD_SPR_OFF) : (r > WASD_SPR_ON);
#endif
        }
        setKey(kw, nw, kUp);
        setKey(ka, na, kLeft);
        setKey(ks, ns, kDown);
        setKey(kd, nd, kRight);
#if WASD_SPRINT_ON_FULL
        setKey(kspr, nspr, KEY_LEFT_CTRL);
#endif
    }

    // Klici ob preklopu nacina, sicer ti tipka obvisi pritisnjena.
    void releaseAll() {
        setKey(kw, false, kUp);    setKey(ka, false, kLeft);
        setKey(ks, false, kDown);  setKey(kd, false, kRight);
#if WASD_SPRINT_ON_FULL
        setKey(kspr, false, KEY_LEFT_CTRL);
#endif
        walking = false;
    }

    bool isWalking() const { return walking; }

private:
    static bool hyst(bool cur, float v) { return cur ? (v > kpAOff()) : (v > kpAOn()); }
    static void setKey(bool &state, bool want, uint8_t code) {
        if (want == state) return;
        state = want;
        if (want) Keyboard.press(code); else Keyboard.release(code);
    }
    uint8_t kUp = 'w', kLeft = 'a', kDown = 's', kRight = 'd';
    bool walking = false, kw = false, ka = false, ks = false, kd = false, kspr = false;
};
