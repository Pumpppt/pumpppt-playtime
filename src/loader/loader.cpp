// pumpppt-playtime.dll: the one DLL you inject, for every chapter.
// Each chapter module (built against its own Dumper-7 SDK) is embedded as an RCDATA resource.
// At runtime this picks the right one for the running game, loads it and calls PM_Start().
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "../resolve.hpp"

namespace Resolve = Mod::Resolve;

namespace
{
    constexpr int kFirstResId = 101;   // ch1 = 101 ... ch5 = 105
    constexpr int kChapters   = 5;

    HMODULE g_self = nullptr;

    void Log(const char* fmt, ...)
    {
        char buf[512];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        std::printf("[pumpppt-playtime] %s\n", buf);
    }

    struct Mod
    {
        int          chapter = 0;
        std::string  tag;
        unsigned     baked   = 0;
        HMODULE      h       = nullptr;
        std::wstring path;
        int        (*start)() = nullptr;
        int          score   = 0;
    };

    std::wstring TempDir()
    {
        wchar_t t[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, t);
        std::wstring d = std::wstring(t) + L"pumpppt-playtime\\";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }

    void PurgeStale(const std::wstring& dir)
    {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"*.dll").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do { DeleteFileW((dir + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));   // locked ones just fail
        FindClose(h);
    }

    bool Extract(int resId, const std::wstring& dst)
    {
        HRSRC r = FindResourceW(g_self, MAKEINTRESOURCEW(resId), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        if (!r) return false;
        HGLOBAL g = LoadResource(g_self, r);
        const DWORD size = SizeofResource(g_self, r);
        const void* data = g ? LockResource(g) : nullptr;
        if (!data || !size) return false;
        HANDLE f = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        const BOOL ok = WriteFile(f, data, size, &wrote, nullptr);
        CloseHandle(f);
        return ok && wrote == size;
    }

    // manual override: env PUMPPPT_CHAPTER=N, or a file "pumpppt-playtime.chapter" next to this DLL containing N
    int ChapterOverride()
    {
        char env[16] = {};
        if (GetEnvironmentVariableA("PUMPPPT_CHAPTER", env, sizeof(env)) && env[0] >= '1' && env[0] <= '5') return env[0] - '0';
        wchar_t p[MAX_PATH] = {};
        GetModuleFileNameW(g_self, p, MAX_PATH);
        std::wstring path(p);
        const size_t slash = path.find_last_of(L"\\/");
        path = path.substr(0, slash == std::wstring::npos ? 0 : slash + 1) + L"pumpppt-playtime.chapter";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"r") == 0 && f)
        {
            const int c = std::fgetc(f);
            std::fclose(f);
            if (c >= '1' && c <= '5') return c - '0';
        }
        return 0;
    }

    DWORD WINAPI Boot(LPVOID)
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
        Log("loader up, unpacking chapter modules");

        const std::wstring dir = TempDir();
        PurgeStale(dir);

        std::vector<Mod> mods;
        for (int n = 1; n <= kChapters; ++n)
        {
            Mod m;
            m.path = dir + std::to_wstring(GetCurrentProcessId()) + L"_ch" + std::to_wstring(n) + L".dll";
            if (!Extract(kFirstResId + n - 1, m.path)) continue;       // this chapter was not built in
            m.h = LoadLibraryW(m.path.c_str());
            if (!m.h) { Log("ch%d: LoadLibrary failed (%lu)", n, GetLastError()); DeleteFileW(m.path.c_str()); continue; }
            auto chapter = reinterpret_cast<int (*)()>(GetProcAddress(m.h, "PM_Chapter"));
            auto tag     = reinterpret_cast<const char* (*)()>(GetProcAddress(m.h, "PM_EngineTag"));
            auto baked   = reinterpret_cast<unsigned (*)()>(GetProcAddress(m.h, "PM_BakedGObjects"));
            m.start      = reinterpret_cast<int (*)()>(GetProcAddress(m.h, "PM_Start"));
            if (!chapter || !tag || !baked || !m.start) { Log("ch%d: module exports missing", n); FreeLibrary(m.h); DeleteFileW(m.path.c_str()); continue; }
            m.chapter = chapter(); m.tag = tag(); m.baked = baked();
            mods.push_back(std::move(m));
        }
        if (mods.empty()) { Log("no chapter modules embedded, nothing to do"); return 1; }

        const HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
        std::string tag;
        const int forced = ChapterOverride();

        if (!forced)
        {
            Log("waiting for the engine to come up");
            Resolve::ObjArrayInfo live{};
            for (int i = 0; i < 600 && !Resolve::ScanObjArray(exe, &live); ++i) Sleep(500);
            tag = Resolve::EngineTag(exe);
            Log("engine tag from exe: %s", tag.empty() ? "(not found)" : tag.c_str());
        }

        for (Mod& m : mods)
        {
            if (forced) { m.score = (m.chapter == forced) ? 1000 : 0; continue; }
            if (!tag.empty() && m.tag == tag) m.score += 10;
            if (Resolve::ValidateObjArray(base + m.baked, nullptr)) m.score += 100;   // SDK offset matches this exe
            Log("ch%d  engine %-5s  SDK GObjects offset %s  score %d", m.chapter, m.tag.c_str(),
                (m.score >= 100) ? "VALID" : "no match", m.score);
        }

        auto best = std::max_element(mods.begin(), mods.end(), [](const Mod& a, const Mod& b)
        {
            return a.score != b.score ? a.score < b.score : a.chapter > b.chapter;   // tie -> lowest chapter
        });
        if (!forced && best->score == 0)
            Log("could not tell which chapter this is (no engine tag, no valid SDK offset). Using ch%d. "
                "Set PUMPPPT_CHAPTER=1..5 if that is wrong.", best->chapter);
        else
            Log("using chapter module ch%d%s", best->chapter, forced ? " (forced)" : "");

        Mod chosen = *best;
        for (Mod& m : mods)
            if (m.h != chosen.h) { FreeLibrary(m.h); DeleteFileW(m.path.c_str()); }

        return static_cast<DWORD>(chosen.start());
    }
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        HANDLE t = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
