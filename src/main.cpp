// Predictive Input Buffer (sodernos) - implementation originale pour Geode (GD 2.2081).
//
// 1) Buffer : un clic fait trop tot (rien a cliquer pour l'instant) est memorise
//    pendant N ticks et rejoue des que le clic devient possible (sol ou orbe).
// 2) Repetition : si "repeat-held" est actif et que tu maintiens le clic, il est
//    re-declenche a chaque nouvelle occasion (atterrissage, nouvel orbe).
// 3) Console live : n'affiche que les changements de fenetre et les SAVED.
//    Tous les evenements sont aussi dans le journal Geode (geode/logs).
// 4) Touche (F4 par defaut, reglable) : fait defiler la fenetre 1, 2, 4, 8, 16, OFF.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

using namespace geode::prelude;

namespace mirb {
    static HANDLE g_out = INVALID_HANDLE_VALUE;   // sortie console (CONOUT$)
    static unsigned long long g_tick = 0;

    static void emit(bool toConsole, char const* fmt, va_list args) {
        char msg[400];
        std::vsnprintf(msg, sizeof(msg), fmt, args);

        auto* pl = PlayLayer::get();
        float pct = pl ? pl->getCurrentPercent() : 0.f;
        char line[512];
        std::snprintf(line, sizeof(line), "[MIRB] tick %-8llu %5.1f%%  %s", g_tick, pct, msg);

        // Toujours dans le journal Geode, meme si la fenetre console n'existe pas.
        log::info("{}", line);

        if (toConsole && g_out != INVALID_HANDLE_VALUE) {
            std::string out = std::string(line) + "\r\n";
            DWORD written = 0;
            WriteConsoleA(g_out, out.c_str(), static_cast<DWORD>(out.size()), &written, nullptr);
        }
    }

    // Affiche dans la console ET le journal : changement de fenetre, SAVED.
    static void say(char const* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        emit(true, fmt, args);
        va_end(args);
    }

    // Journal Geode seulement (la console reste propre).
    static void note(char const* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        emit(false, fmt, args);
        va_end(args);
    }

    static bool enabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
    static bool repeatHeld() { return Mod::get()->getSettingValue<bool>("repeat-held"); }
    static int windowTicks() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("window-ticks")); }

    // OFF -> 1 -> 2 -> 4 -> 8 -> 16 -> OFF -> ...
    static void cycleWindow() {
        static int const steps[] = {1, 2, 4, 8, 16};
        auto* mod = Mod::get();

        if (!enabled()) {
            mod->setSettingValue<bool>("enabled", true);
            mod->setSettingValue<int64_t>("window-ticks", steps[0]);
            say("WINDOW    -> %d tick(s) (%.2f frame @60fps)", steps[0], steps[0] / 4.0);
            return;
        }

        int cur = windowTicks();
        int next = -1;
        for (int s : steps) {
            if (s > cur) { next = s; break; }
        }

        if (next < 0) {
            mod->setSettingValue<bool>("enabled", false);
            say("WINDOW    -> OFF (no buffering, no repeat)");
        } else {
            mod->setSettingValue<int64_t>("window-ticks", next);
            say("WINDOW    -> %d ticks (%.2f frames @60fps)", next, next / 4.0);
        }
    }
}

$on_mod(Loaded) {
    if (!Mod::get()->getSettingValue<bool>("console")) return;

    // On n'utilise pas freopen sur stdout : Geode lit stdout par un tuyau et
    // le casserait. On ecrit directement dans la console avec WriteConsole.
    BOOL allocated = AllocConsole();
    DWORD err = allocated ? 0 : GetLastError();

    HANDLE h = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log::warn("Predictive Input Buffer: could not open a console (AllocConsole={}, error={})",
                  allocated ? 1 : 0, static_cast<unsigned long>(err));
        return;
    }
    mirb::g_out = h;

    if (allocated) SetConsoleTitleA("Predictive Input Buffer - Live Console");

    if (HWND hwnd = GetConsoleWindow()) {
        // Fermer cette fenetre fermerait le jeu : on grise le bouton X.
        if (HMENU menu = GetSystemMenu(hwnd, FALSE)) {
            EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
        }
        // Au premier plan pour ne pas rester cachee derriere le jeu.
        SetWindowPos(hwnd, HWND_TOPMOST, 40, 40, 900, 420, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }

    {
        char const* header = "=== Predictive Input Buffer - Mod made by Sodernos ===\r\n";
        DWORD written = 0;
        WriteConsoleA(h, header, static_cast<DWORD>(std::strlen(header)), &written, nullptr);
        log::info("Mod made by Sodernos");
    }
    mirb::note("console ready (AllocConsole=%d err=%lu, window=%d ticks, repeat=%d)",
              allocated ? 1 : 0, static_cast<unsigned long>(err),
              mirb::windowTicks(), mirb::repeatHeld() ? 1 : 0);
}

// Touche de defilement de la fenetre (reglage "cycle-key", F4 par defaut).
$execute {
    listenForKeybindSettingPresses("cycle-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) mirb::cycleWindow();
    });
}

class $modify(MirbPlayer, PlayerObject) {
    struct Fields {
        bool pending = false;      // un clic trop tot attend son occasion
        bool held = false;         // le bouton est maintenu
        bool wasCan = false;       // un clic etait possible au tick precedent
        bool releaseNext = false;  // relacher le clic rejoue au tick suivant (tap)
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

    void clearState() {
        m_fields->pending = false;
        m_fields->held = false;
        m_fields->wasCan = false;
        m_fields->releaseNext = false;
        m_fields->ticksLeft = 0;
        m_fields->waited = 0;
    }

    bool pushButton(PlayerButton btn) {
        if (btn == PlayerButton::Jump && mirb::enabled() && this->modeIsBuffered()) {
            // Seul un VRAI nouvel appui compte (pas un appui repete alors qu'on tient deja).
            bool newPress = !m_fields->held;
            m_fields->held = true;
            if (newPress && !this->canClickNow()) {
                m_fields->pending = true;
                m_fields->ticksLeft = mirb::windowTicks();
                m_fields->waited = 0;
                mirb::note("BUFFERED  early click, window = %d ticks", m_fields->ticksLeft);
            }
        }
        return PlayerObject::pushButton(btn);
    }

    void resetObject() {
        PlayerObject::resetObject();
        this->clearState();
    }

    // Relacher NE supprime PAS le clic en attente : un tap trop tot sera rejoue.
    bool releaseButton(PlayerButton btn) {
        if (btn == PlayerButton::Jump) m_fields->held = false;
        return PlayerObject::releaseButton(btn);
    }

    void update(float dt) {
        PlayerObject::update(dt);

        auto* pl = PlayLayer::get();
        if (!pl) return;
        if (this == pl->m_player1) mirb::g_tick++;

        // Fin du tap rejoue au tick precedent.
        if (m_fields->releaseNext) {
            m_fields->releaseNext = false;
            if (!m_fields->held) PlayerObject::releaseButton(PlayerButton::Jump);
        }

        if (!mirb::enabled()) { m_fields->pending = false; return; }

        if (!this->modeIsBuffered()) {
            m_fields->pending = false;
            m_fields->wasCan = false;
            return;
        }

        bool can = this->canClickNow();
        bool fired = false;

        if (m_fields->pending) {
            m_fields->waited++;
            if (can) {
                if (m_fields->held) PlayerObject::releaseButton(PlayerButton::Jump);
                PlayerObject::pushButton(PlayerButton::Jump);
                if (!m_fields->held) m_fields->releaseNext = true;
                mirb::say("SAVED     click delayed by %d tick(s) (%.2f frame @60fps)",
                          m_fields->waited, m_fields->waited / 4.0);
                m_fields->pending = false;
                fired = true;
            } else if (--m_fields->ticksLeft <= 0) {
                mirb::note("EXPIRED   no chance within the window");
                m_fields->pending = false;
            }
        }

        // Repetition : bouton maintenu + nouvelle occasion = nouveau clic.
        if (!fired && can && !m_fields->wasCan && m_fields->held && mirb::repeatHeld()) {
            PlayerObject::releaseButton(PlayerButton::Jump);
            PlayerObject::pushButton(PlayerButton::Jump);
            mirb::note("REPEAT    held click re-fired on a new chance");
        }

        m_fields->wasCan = can;
    }
};

// Mort / restart / checkpoint : on relache le clic des deux joueurs et on oublie
// tout etat, sinon le joueur "tient" le clic tout seul apres la reapparition.
class $modify(MirbPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        for (auto* p : {m_player1, m_player2}) {
            if (!p) continue;
            p->releaseButton(PlayerButton::Jump);
            static_cast<MirbPlayer*>(p)->clearState();
        }
        mirb::note("RESET     new attempt, jump released on both players");
    }
};
