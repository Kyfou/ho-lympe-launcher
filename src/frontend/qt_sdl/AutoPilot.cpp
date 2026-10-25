/*
    HeartGold Coop - autopilot for automated testing.

    Script format (one command per line, # for comments):
      wait N                 do nothing for N frames
      press A+UP N           hold buttons for N frames (A B X Y L R START SELECT UP DOWN LEFT RIGHT)
      tap A                  short press (4 frames down, 8 frames up)
      touch X Y N            touch the bottom screen at (X, Y) for N frames
      shot NAME              save both screens to <HGCOOP_SHOTS>/NAME.png
      quit                   close the emulator

    Environment: HGCOOP_SCRIPT = script file, HGCOOP_SHOTS = screenshot folder
    (defaults to the script's folder). Progress is logged to <HGCOOP_SHOTS>/autopilot.log.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <sstream>

#include <QApplication>
#include <QImage>
#include <QPainter>

#include "AutoPilot.h"
#include "GameUi.h"
#include <QWidget>
#include "NDS.h"

using namespace melonDS;

static u32 ParseButtons(const std::string& text)
{
    static const char* names[12] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};
    u32 mask = 0;
    std::stringstream ss(text);
    std::string part;
    while (std::getline(ss, part, '+'))
    {
        for (int i = 0; i < 12; i++)
        {
            if (part == names[i])
                mask |= 1 << i;
        }
    }
    return mask;
}

AutoPilot::AutoPilot()
{
    const char* script = getenv("HGCOOP_SCRIPT");
    if (!script)
        return;

    std::string path = script;
    const char* shots = getenv("HGCOOP_SHOTS");
    if (shots)
        shotDir = shots;
    else
    {
        size_t slash = path.find_last_of("/\\");
        shotDir = slash == std::string::npos ? "." : path.substr(0, slash);
    }
    logPath = shotDir + "/autopilot.log";
    remove(logPath.c_str());

    active = load(path);
    log("autopilot %s: %zu steps\n", active ? "started" : "failed to load script", steps.size());
}

bool AutoPilot::load(const std::string& path)
{
    std::ifstream file(path);
    if (!file)
        return false;

    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::stringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd) || cmd[0] == '#')
            continue;

        Step step {};
        if (cmd == "wait")
        {
            step.type = Step::Wait;
            ss >> step.frames;
        }
        else if (cmd == "press")
        {
            std::string buttons;
            ss >> buttons >> step.frames;
            step.type = Step::Press;
            step.buttons = ParseButtons(buttons);
        }
        else if (cmd == "tap")
        {
            std::string buttons;
            ss >> buttons;
            step.type = Step::Press;
            step.buttons = ParseButtons(buttons);
            step.frames = 4;
            steps.push_back(step);
            step = {};
            step.type = Step::Wait;
            step.frames = 8;
        }
        else if (cmd == "touch")
        {
            step.type = Step::Touch;
            ss >> step.x >> step.y >> step.frames;
        }
        else if (cmd == "shot")
        {
            step.type = Step::Shot;
            ss >> step.name;
        }
        else if (cmd == "winshot")
        {
            // Saves the whole window (HD screens included) to <shots>/NAME.png
            step.type = Step::WindowShot;
            ss >> step.name;
        }
        else if (cmd == "click")
        {
            // A mouse click on the HD screens at (X, Y) in percent of the window
            step.type = Step::Click;
            ss >> step.x >> step.y;
        }
        else if (cmd == "dumppal")
        {
            // Saves the DS palette RAM (main BG, main OBJ, sub BG, sub OBJ) to <shots>/NAME.pal
            step.type = Step::DumpPalette;
            ss >> step.name;
        }
        else if (cmd == "sayto")
        {
            // Chat: a private message, "sayto NAME TEXT"
            step.type = Step::Say;
            ss >> step.to;
            std::getline(ss, step.name);
            if (!step.name.empty() && step.name[0] == ' ')
                step.name.erase(0, 1);
        }
        else if (cmd == "nametype")
        {
            // Naming box: types TEXT (confirm with "tap A")
            step.type = Step::NameType;
            std::getline(ss, step.name);
            if (!step.name.empty() && step.name[0] == ' ')
                step.name.erase(0, 1);
        }
        else if (cmd == "say" || cmd == "chatbox")
        {
            // Chat: sends TEXT to everybody / shows the open chat box with TEXT
            step.type = cmd == "say" ? Step::Say : Step::ChatBox;
            std::getline(ss, step.name);
            if (!step.name.empty() && step.name[0] == ' ')
                step.name.erase(0, 1);
        }
        else if (cmd == "quit")
            step.type = Step::Quit;
        else
            continue;
        steps.push_back(step);
    }
    return true;
}

void AutoPilot::log(const char* fmt, ...)
{
    FILE* f = fopen(logPath.c_str(), "a");
    if (!f)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

void AutoPilot::ApplyInput(NDS* nds)
{
    if (!active)
        return;

    u32 buttons = 0;
    bool touching = false;
    if (current < steps.size())
    {
        const Step& step = steps[current];
        if (step.type == Step::Press)
            buttons = step.buttons;
        else if (step.type == Step::Touch)
            touching = true;
    }

    nds->SetKeyMask(0xFFF & ~buttons); // the DS uses 0 for "pressed"
    if (touching)
        nds->TouchScreen(steps[current].x, steps[current].y);
    else
        nds->ReleaseScreen();
}

void AutoPilot::AfterFrame(NDS* nds)
{
    if (!active || current >= steps.size())
        return;

    Step& step = steps[current];
    switch (step.type)
    {
    case Step::Shot:
        saveScreenshot(nds, step.name);
        current++;
        return;

    case Step::WindowShot:
    {
        std::string path = shotDir + "/" + step.name + ".png";
        QMetaObject::invokeMethod(qApp, [path]() {
            for (QWidget* w : QApplication::topLevelWidgets())
            {
                if (w->isVisible() && w->inherits("QMainWindow"))
                {
                    w->grab().save(QString::fromStdString(path));
                    break;
                }
            }
        }, Qt::QueuedConnection);
        log("winshot %s\n", step.name.c_str());
        current++;
        return;
    }

    case Step::Click:
    {
        bool hit = GameUi::Get().HandleClick(step.x / 100.0f, step.y / 100.0f);
        log(hit ? "click: hit\n" : "click: miss\n");
    }
        current++;
        return;

    case Step::DumpPalette:
    {
        std::string path = shotDir + "/" + step.name + ".pal";
        if (FILE* f = fopen(path.c_str(), "wb"))
        {
            fwrite(nds->GPU.Palette, 1, sizeof(nds->GPU.Palette), f);
            fclose(f);
        }
        log("dumppal %s\n", step.name.c_str());
        current++;
        return;
    }

    case Step::NameType:
        GameUi::Get().NamingType(QString::fromStdString(step.name));
        log("nametype %s\n", step.name.c_str());
        current++;
        return;

    case Step::Say:
    case Step::ChatBox:
        if (step.type == Step::Say)
            GameUi::Get().ChatSay(QString::fromStdString(step.name), QString::fromStdString(step.to));
        else
            GameUi::Get().ChatShowInput(QString::fromStdString(step.name));
        log("%s %s\n", step.type == Step::Say ? "say" : "chatbox", step.name.c_str());
        current++;
        return;

    case Step::Quit:
        log("quit\n");
        active = false;
        QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
        return;

    default:
        if (framesLeft < 0)
            framesLeft = step.frames;
        if (--framesLeft <= 0)
        {
            framesLeft = -1;
            current++;
            if (current >= steps.size())
                log("script finished\n");
        }
        return;
    }
}

void AutoPilot::saveScreenshot(NDS* nds, const std::string& name)
{
    void* top = nullptr;
    void* bottom = nullptr;
    if (!nds->GPU.GetFramebuffers(&top, &bottom))
    {
        log("shot %s: no RAM framebuffer (use the software renderer)\n", name.c_str());
        return;
    }

    QImage topImg((const uchar*)top, 256, 192, QImage::Format_RGB32);
    QImage bottomImg((const uchar*)bottom, 256, 192, QImage::Format_RGB32);
    QImage both(256, 384, QImage::Format_RGB32);
    QPainter painter(&both);
    painter.drawImage(0, 0, topImg);
    painter.drawImage(0, 192, bottomImg);
    painter.end();

    std::string path = shotDir + "/" + name + ".png";
    bool ok = both.save(QString::fromStdString(path));
    log("shot %s: %s\n", name.c_str(), ok ? "saved" : "save failed");
}
