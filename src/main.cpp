// Predictive Input Buffer (sodernos) - implementation originale pour Geode (GD 2.2081).
//
// 1) Buffer : un clic fait trop tot (rien a cliquer pour l'instant) est memorise
//    pendant N ticks et rejoue des que le clic devient possible (sol ou orbe).
// 2) Repetition : si "repeat-held" est actif et que tu maintiens le clic, il est
//    re-declenche a chaque nouvelle occasion (atterrissage, nouvel orbe).
// 3) Console live : une fenetre Windows qui affiche chaque evenement.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Windows.h>
#include <cstdarg>
#include <cstdio>

using namespace geode::prelude;

namespace mirb {
    static bool g_console = false;
    static unsigned long long g_tick = 0;

    static void say(char const* fmt, ...) {
        if (!g_console) return;
        auto* pl = PlayLayer::get();
        float pct = pl ? pl->getCurrentPercent() : 0.f;
        std::printf("[MIRB] tick %-8llu %5.1f%%  ", g_tick, pct);
        va_list args;
        va_start(args, fmt);
        std::vprintf(fmt, args);
        va_end(args);
        std::printf("\n");
        std::fflush(stdout);
    }

    static bool enabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
    static bool repeatHeld() { return Mod::get()->getSettingValue<bool>("repeat-held"); }
    static int windowTicks() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("window-ticks")); }
}

$on_mod(Loaded) {
    if (!Mod::get()->getSettingValue<bool>("console")) return;
    if (AllocConsole()) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        SetConsoleTitleA("Predictive Input Buffer - Live Console");
        // Fermer cette fenetre fermerait le jeu : on grise le bouton X.
        if (HWND hwnd = GetConsoleWindow()) {
            if (HMENU menu = GetSystemMenu(hwnd, FALSE)) {
                EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
            }
        }
        mirb::g_console = true;
        mirb::say("console ready (window=%d ticks, repeat=%d)", mirb::windowTicks(), mirb::repeatHeld() ? 1 : 0);
    }
}

class $modify(MirbPlayer, PlayerObject) {
    struct Fields {
        bool pending = false;   // un clic precoce attend son occasion
        bool held = false;      // le bouton est maintenu
        bool wasCan = false;    // un clic etait possible au tick precedent
        int ticksLeft = 0;
        int waited = 0;
    };

    // Ship, UFO, wave et swing n'ont pas de "sol" a attendre : on n'y touche pas.
    bool modeIsBuffered() {
        return !(m_isShip || m_isBird || m_isDart || m_isSwing);
    }

    // Vrai si un clic aurait un effet maintenant : au sol ou au contact d'un orbe.
    bool canClickNow() {
        if (m_isOnGround) return true;
        return m_touchingRings && m_touchingRings->count() > 0;
    }

    bool pushButton(PlayerButton btn) {
        if (btn == PlayerButton::Jump && mirb::enabled() && this->modeIsBuffered()) {
            m_fields->held = true;
            if (!this->canClickNow()) {
                m_fields->pending = true;
                m_fields->ticksLeft = mirb::windowTicks();
                m_fields->waited = 0;
                mirb::say("BUFFERED  early click, window = %d ticks", m_fields->ticksLeft);
            }
        }
        return PlayerObject::pushButton(btn);
    }

    // Nouvelle tentative (mort, restart, checkpoint) : on oublie tout etat de maintien,
    // sinon le mod continuerait a "tenir" le clic a ta place jusqu'a ton prochain clic.
    void resetObject() {
        PlayerObject::resetObject();
        m_fields->pending = false;
        m_fields->held = false;
        m_fields->wasCan = false;
        m_fields->ticksLeft = 0;
        m_fields->waited = 0;
        mirb::say("RESET     new attempt, held state cleared");
    }

    bool releaseButton(PlayerButton btn) {
        if (btn == PlayerButton::Jump) {
            m_fields->held = false;
            if (m_fields->pending) {
                m_fields->pending = false;
                mirb::say("CANCELLED released before the chance came");
            }
        }
        return PlayerObject::releaseButton(btn);
    }

    void update(float dt) {
        PlayerObject::update(dt);

        auto* pl = PlayLayer::get();
        if (!pl || !mirb::enabled()) return;
        if (this == pl->m_player1) mirb::g_tick++;

        if (!this->modeIsBuffered()) {
            m_fields->pending = false;
            return;
        }

        bool can = this->canClickNow();
        bool fired = false;

        if (m_fields->pending) {
            m_fields->waited++;
            if (can) {
                PlayerObject::pushButton(PlayerButton::Jump);
                mirb::say("FIRED     buffered click after %d ticks", m_fields->waited);
                m_fields->pending = false;
                fired = true;
            } else if (--m_fields->ticksLeft <= 0) {
                mirb::say("EXPIRED   no chance within the window");
                m_fields->pending = false;
            }
        }

        // Repetition : bouton maintenu + nouvelle occasion = nouveau clic.
        if (!fired && can && !m_fields->wasCan && m_fields->held && mirb::repeatHeld()) {
            PlayerObject::releaseButton(PlayerButton::Jump);
            PlayerObject::pushButton(PlayerButton::Jump);
            mirb::say("REPEAT    held click re-fired on a new chance");
        }

        m_fields->wasCan = can;
    }
};
