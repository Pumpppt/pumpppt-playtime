#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#ifndef MOD_CHAPTER
#define MOD_CHAPTER 0
#endif

namespace Mod
{
    struct State
    {
        // UI / input
        std::atomic<bool>  menuOpen{true};
        std::atomic<bool>  uiWantsKeyboard{false};
        std::atomic<void*> gameHwnd{nullptr};

        // Console
        std::atomic<bool>  unlockConsole{true};

        // Camera
        std::atomic<bool>  fovEnabled{false};
        std::atomic<float> fov{100.0f};

        // Fly
        std::atomic<bool>  fly{false};
        std::atomic<float> flySpeed{1500.0f};
        std::atomic<float> flyBoost{3.0f};
        std::atomic<bool>  flyNoclip{true};

        // Speeds
        std::atomic<bool>  gameSpeedEnabled{false};
        std::atomic<float> gameSpeed{1.0f};
        std::atomic<bool>  playerSpeedEnabled{false};
        std::atomic<float> playerSpeed{1.0f};

        // Aim / delete
        std::atomic<int>   traceChannel{0};
        std::atomic<float> traceDistance{20000.0f};
        std::atomic<bool>  hardDestroy{false};
        std::atomic<bool>  reqDelete{false};
        std::atomic<bool>  reqUndo{false};
        std::atomic<bool>  reqUndoAll{false};

        // Text shared between threads
        std::mutex                mtx;
        std::string               targetName  = "<none>";
        std::string               targetClass = "<none>";
        std::string               worldName   = "<none>";
        std::string               consoleStatus = "pending";
        int                       hiddenCount = 0;
        std::vector<std::wstring> cmdQueue;
        std::deque<std::string>   logLines;
    };

    extern State g;
    extern void* g_exeBase;   // real exe module, captured before any base heal is installed

    inline void Log(const char* fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::printf("[pumpppt-playtime] %s\n", buf);
        std::lock_guard<std::mutex> lk(g.mtx);
        g.logLines.emplace_back(buf);
        while (g.logLines.size() > 200) g.logLines.pop_front();
    }

    namespace Features { bool Install(); unsigned BakedGObjects(); }
    namespace Render   { bool Install(); }
    namespace Menu     { void Draw();  }
}
