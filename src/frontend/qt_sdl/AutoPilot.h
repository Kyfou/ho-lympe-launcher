/*
    HeartGold Coop - autopilot for automated testing.

    Plays a script of button presses and saves screenshots, so the game can be
    tested without anyone at the controls. Enabled with HGCOOP_SCRIPT=<file>.
*/

#ifndef AUTOPILOT_H
#define AUTOPILOT_H

#include <string>
#include <vector>
#include "types.h"

namespace melonDS { class NDS; }

class AutoPilot
{
public:
    AutoPilot();

    bool IsActive() const { return active; }

    // Before NDS::RunFrame(): overrides the buttons and touch screen
    void ApplyInput(melonDS::NDS* nds);
    // After NDS::RunFrame(): takes screenshots, advances the script
    void AfterFrame(melonDS::NDS* nds);

private:
    struct Step
    {
        enum Type { Wait, Press, Touch, Shot, WindowShot, DumpPalette, Click, Say, ChatBox, NameType, Quit } type;
        melonDS::u32 buttons = 0; // DS key bits, 1 = pressed
        int frames = 0;
        int x = 0, y = 0;
        std::string name;
        std::string to; // sayto: the recipient
    };

    bool active = false;
    std::vector<Step> steps;
    size_t current = 0;
    int framesLeft = -1;
    std::string shotDir;
    std::string logPath;

    bool load(const std::string& path);
    void log(const char* fmt, ...);
    void saveScreenshot(melonDS::NDS* nds, const std::string& name);
};

#endif // AUTOPILOT_H
