#include <Windows.h>
#include <cstdio>
#include "MinHook.h"
#include "shared.hpp"

#ifndef MOD_ENGINE_TAG
#define MOD_ENGINE_TAG ""
#endif

namespace Mod { State g; void* g_exeBase = nullptr; }

// ---- exports used by the single-DLL loader (pumpppt-playtime.dll) ---------------------------
extern "C" __declspec(dllexport) int         PM_Chapter()       { return MOD_CHAPTER; }
extern "C" __declspec(dllexport) const char* PM_EngineTag()     { return MOD_ENGINE_TAG; }
extern "C" __declspec(dllexport) unsigned    PM_BakedGObjects() { return Mod::Features::BakedGObjects(); }

extern "C" __declspec(dllexport) int PM_Start()
{
    if (!GetConsoleWindow())
    {
        AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$",  "r", stdin);
        SetConsoleTitleA("pumpppt-playtime");
    }

    // Must happen before the SDK heal hook exists, otherwise this would be answered by the hook.
    Mod::g_exeBase = GetModuleHandleW(nullptr);

    Mod::Log("pumpppt-playtime loaded, SDK = chapter %d (engine %s)", MOD_CHAPTER, MOD_ENGINE_TAG);

    if (MH_Initialize() != MH_OK)
    {
        Mod::Log("MinHook init failed");
        return 1;
    }

    if (!Mod::Features::Install())
        Mod::Log("Feature hook failed, game-side features disabled");

    if (!Mod::Render::Install())
        Mod::Log("Render hook failed, no menu");

    Mod::Log("Ready. INSERT = menu | F6 = fly | DELETE = delete aimed | F7 = undo");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(mod);   // nothing else: the loader calls PM_Start
    return TRUE;
}
