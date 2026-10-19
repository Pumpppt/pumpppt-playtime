// Game-thread features. One source, compiled against each chapter's Dumper-7 SDK.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

#include <intrin.h>
#include "SDK.hpp"
#include "MinHook.h"
#include "shared.hpp"
#include "resolve.hpp"

using namespace SDK;

namespace Mod::Features
{
    namespace
    {
        constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

        using ProcessEvent_t = void (*)(const UObject*, UFunction*, void*);
        ProcessEvent_t g_origProcessEvent = nullptr;

        DWORD                 g_gameThreadId = 0;
        thread_local bool     t_inTick       = false;
        LARGE_INTEGER         g_qpcFreq{};
        LARGE_INTEGER         g_lastTick{};
        unsigned long long    g_tickCount    = 0;
        std::atomic<unsigned long long> g_peCalls{0};
        std::atomic<unsigned long long> g_wrongThread{0};
        std::atomic<unsigned long long> g_ticksRun{0};
        std::atomic<int>                g_faults{0};
        const char*                     g_stage = "not started";

        void Stage(const char* why)
        {
            if (why != g_stage)
            {
                g_stage = why;
                Log("stage: %s", why);
            }
        }

        // ---- per-feature state -------------------------------------------------
        bool   s_menuWasOpen   = false;
        bool   s_flyActive     = false;
        bool   s_fovWasOn      = false;
        float  s_fovBaseline   = 90.0f;
        float  s_fovLast       = 0.0f;
        bool   s_gsWasOn       = false;
        float  s_gsLast        = 1.0f;
        bool   s_psWasOn       = false;
        UWorld* s_lastWorld    = nullptr;
        bool   s_keysPatched   = false;

        struct SpeedSlot { float base = 0.f; float written = 0.f; bool valid = false; };
        UCharacterMovementComponent* s_cmc = nullptr;
        SpeedSlot s_walk, s_crouch, s_fly;

        struct Hidden { AActor* actor; bool destroyed; };
        std::vector<Hidden> s_hidden;

        // ---- helpers -----------------------------------------------------------
        bool KeyDown(int vk)
        {
            HWND game = static_cast<HWND>(g.gameHwnd.load());
            if (!game || GetForegroundWindow() != game) return false;
            return (GetAsyncKeyState(vk) & 0x8000) != 0;
        }

        bool ActorAlive(AActor* a)
        {
            if (!a) return false;
            UObject* slot = UObject::GObjects->GetByIndex(a->Index);
            return slot == a;
        }

        void SetText(std::string& dst, const std::string& v)
        {
            std::lock_guard<std::mutex> lk(g.mtx);
            dst = v;
        }

        // ---- console unlock ----------------------------------------------------
        void EnsureConsole(ULocalPlayer* lp)
        {
            UGameViewportClient* vp = lp->ViewportClient;
            if (!vp) return;

            if (!vp->ViewportConsole)
            {
                UEngine* engine = UEngine::GetEngine();
                if (!engine || !engine->ConsoleClass.Get())
                {
                    SetText(g.consoleStatus, "no ConsoleClass on UEngine");
                    return;
                }
                UObject* console = UGameplayStatics::SpawnObject(engine->ConsoleClass, vp);
                vp->ViewportConsole = static_cast<UConsole*>(console);
                Log("ViewportConsole spawned at %p", static_cast<void*>(vp->ViewportConsole));
            }

            if (!s_keysPatched)
            {
                UInputSettings* settings = UInputSettings::GetInputSettings();
                if (settings)
                {
                    FName tilde = UKismetStringLibrary::Conv_StringToName(FString(L"Tilde"));
                    FName f2    = UKismetStringLibrary::Conv_StringToName(FString(L"F2"));
                    const int n = settings->ConsoleKeys.Num();
                    if (n >= 1) settings->ConsoleKeys[0].KeyName = tilde;
                    if (n >= 2) settings->ConsoleKeys[1].KeyName = f2;
                    s_keysPatched = true;
                    Log("ConsoleKeys patched (%d entries). Open with ~ or use the Console tab.", n);
                }
            }
            SetText(g.consoleStatus, vp->ViewportConsole ? "unlocked" : "failed");
        }

        // ---- speed -------------------------------------------------------------
        void ApplySlot(float& field, SpeedSlot& slot, float mul)
        {
            if (!slot.valid || field != slot.written)
            {
                slot.base  = field;       // the game changed it (or first sight), adopt as new baseline
                slot.valid = true;
            }
            slot.written = slot.base * mul;
            field        = slot.written;
        }

        void RestoreSlot(float& field, SpeedSlot& slot)
        {
            if (slot.valid && field == slot.written) field = slot.base;
            slot.valid = false;
        }

        void TickPlayerSpeed(ACharacter* ch)
        {
            UCharacterMovementComponent* cmc = ch ? ch->CharacterMovement : nullptr;
            if (cmc != s_cmc)
            {
                s_cmc = cmc;
                s_walk = s_crouch = s_fly = SpeedSlot{};
            }
            if (!cmc) return;

            const bool on = g.playerSpeedEnabled.load();
            if (on)
            {
                const float mul = g.playerSpeed.load();
                ApplySlot(cmc->MaxWalkSpeed,         s_walk,   mul);
                ApplySlot(cmc->MaxWalkSpeedCrouched, s_crouch, mul);
                ApplySlot(cmc->MaxFlySpeed,          s_fly,    mul);
                s_psWasOn = true;
            }
            else if (s_psWasOn)
            {
                RestoreSlot(cmc->MaxWalkSpeed,         s_walk);
                RestoreSlot(cmc->MaxWalkSpeedCrouched, s_crouch);
                RestoreSlot(cmc->MaxFlySpeed,          s_fly);
                s_psWasOn = false;
            }
        }

        // ---- fly / noclip ------------------------------------------------------
        void StopFly(ACharacter* ch)
        {
            if (!s_flyActive) return;
            s_flyActive = false;
            if (!ch) return;
            ch->SetActorEnableCollision(true);
            if (ch->CharacterMovement)
                ch->CharacterMovement->SetMovementMode(EMovementMode::MOVE_Falling, 0);
        }

        void TickFly(UWorld* world, APlayerController* pc, ACharacter* ch, double dt)
        {
            if (!g.fly.load() || !ch || !ch->CharacterMovement || !pc->PlayerCameraManager)
            {
                StopFly(ch);
                return;
            }

            UCharacterMovementComponent* cmc = ch->CharacterMovement;
            if (!s_flyActive)
            {
                s_flyActive = true;
                if (g.flyNoclip.load()) ch->SetActorEnableCollision(false);
            }
            if (cmc->MovementMode != EMovementMode::MOVE_Flying)
                cmc->SetMovementMode(EMovementMode::MOVE_Flying, 0);

            cmc->Velocity.X = 0; cmc->Velocity.Y = 0; cmc->Velocity.Z = 0;

            if (g.menuOpen.load() || g.uiWantsKeyboard.load()) return;

            const FRotator rot = pc->PlayerCameraManager->GetCameraRotation();
            const double pitch = rot.Pitch * kDegToRad;
            const double yaw   = rot.Yaw   * kDegToRad;

            const double fx = std::cos(pitch) * std::cos(yaw);
            const double fy = std::cos(pitch) * std::sin(yaw);
            const double fz = std::sin(pitch);
            const double rx = -std::sin(yaw);
            const double ry =  std::cos(yaw);

            double mx = 0, my = 0, mz = 0;
            if (KeyDown('W'))         { mx += fx; my += fy; mz += fz; }
            if (KeyDown('S'))         { mx -= fx; my -= fy; mz -= fz; }
            if (KeyDown('D'))         { mx += rx; my += ry; }
            if (KeyDown('A'))         { mx -= rx; my -= ry; }
            if (KeyDown(VK_SPACE))    { mz += 1.0; }
            if (KeyDown(VK_LCONTROL)) { mz -= 1.0; }

            const double len = std::sqrt(mx * mx + my * my + mz * mz);
            if (len < 1e-6) return;

            double speed = g.flySpeed.load();
            if (KeyDown(VK_LSHIFT)) speed *= g.flyBoost.load();
            const double step = speed * dt / len;

            FVector loc = ch->K2_GetActorLocation();
            FVector dst;
            dst.X = loc.X + mx * step;
            dst.Y = loc.Y + my * step;
            dst.Z = loc.Z + mz * step;

            FHitResult hit;
            std::memset(&hit, 0, sizeof(hit));
            ch->K2_SetActorLocation(dst, false, &hit, true);
            (void)world;
        }

        // ---- aim / delete ------------------------------------------------------
        AActor* TraceAim(UWorld* world, APlayerController* pc, ACharacter* ch)
        {
            if (!pc->PlayerCameraManager) return nullptr;

            const FVector   s = pc->PlayerCameraManager->GetCameraLocation();
            const FRotator  r = pc->PlayerCameraManager->GetCameraRotation();
            const double    p = r.Pitch * kDegToRad;
            const double    y = r.Yaw   * kDegToRad;
            const double    d = g.traceDistance.load();

            FVector e;
            e.X = s.X + std::cos(p) * std::cos(y) * d;
            e.Y = s.Y + std::cos(p) * std::sin(y) * d;
            e.Z = s.Z + std::sin(p) * d;

            TArray<AActor*> ignore;
            FHitResult      hit;
            FLinearColor    color;
            std::memset(&hit,   0, sizeof(hit));
            std::memset(&color, 0, sizeof(color));

            const UObject* ctx = ch ? static_cast<const UObject*>(ch) : static_cast<const UObject*>(world);
            const bool blocked = UKismetSystemLibrary::LineTraceSingle(
                ctx, s, e,
                static_cast<ETraceTypeQuery>(g.traceChannel.load()),
                false, ignore, EDrawDebugTrace::None, &hit,
                ch != nullptr, color, color, 0.0f);

            if (!blocked) return nullptr;
            UPrimitiveComponent* comp = hit.Component.Get();
            return comp ? comp->GetOwner() : nullptr;
        }

        void PublishTarget(AActor* a)
        {
            std::string name = "<none>", cls = "<none>";
            if (a)
            {
                name = a->GetName();
                if (a->Class) cls = a->Class->GetName();
            }
            std::lock_guard<std::mutex> lk(g.mtx);
            g.targetName  = std::move(name);
            g.targetClass = std::move(cls);
            g.hiddenCount = static_cast<int>(s_hidden.size());
        }

        void DeleteTarget(AActor* target, ACharacter* ch)
        {
            if (!ActorAlive(target) || target == ch)
            {
                Log("Delete skipped: no valid target");
                return;
            }
            const std::string nm = target->GetName();
            if (g.hardDestroy.load())
            {
                target->K2_DestroyActor();
                Log("Destroyed %s", nm.c_str());
            }
            else
            {
                target->SetActorHiddenInGame(true);
                target->SetActorEnableCollision(false);
                s_hidden.push_back({ target, false });
                Log("Hidden %s (F7 to undo)", nm.c_str());
            }
        }

        void UndoHidden(bool all)
        {
            while (!s_hidden.empty())
            {
                Hidden h = s_hidden.back();
                s_hidden.pop_back();
                if (ActorAlive(h.actor))
                {
                    h.actor->SetActorHiddenInGame(false);
                    h.actor->SetActorEnableCollision(true);
                    Log("Restored %s", h.actor->GetName().c_str());
                }
                if (!all) break;
            }
        }

        // The dumped GWorld offset is "decoration" per Dumper-7, so go through the engine first.
        UWorld* FindWorld()
        {
            if (UEngine* engine = UEngine::GetEngine())
                if (engine->GameViewport && engine->GameViewport->World)
                    return engine->GameViewport->World;
            return UWorld::GetWorld();
        }

        // ---- main tick ---------------------------------------------------------
        void GameTick(double dt)
        {
            UWorld* world = FindWorld();
            if (!world)                       { Stage("GetWorld() is null (main menu or wrong GWorld offset)"); return; }
            if (!world->OwningGameInstance)   { Stage("world has no OwningGameInstance"); return; }

            auto& players = world->OwningGameInstance->LocalPlayers;
            if (players.Num() < 1 || !players[0]) { Stage("no LocalPlayers yet"); return; }

            ULocalPlayer*      lp = players[0];
            APlayerController* pc = lp->PlayerController;
            if (!pc)                          { Stage("LocalPlayer has no PlayerController"); return; }
            Stage("running");
            ++g_ticksRun;

            if (world != s_lastWorld)
            {
                s_lastWorld = world;
                s_hidden.clear();
                s_cmc = nullptr;
                s_flyActive = false;
                s_keysPatched = false;
                s_gsLast = 1.0f;
                SetText(g.worldName, world->GetName());
                Log("World: %s", world->GetName().c_str());
            }

            ACharacter* ch = UGameplayStatics::GetPlayerCharacter(world, 0);

            // console
            if (g.unlockConsole.load() && (g_tickCount % 60) == 0) EnsureConsole(lp);

            // menu <-> input capture
            const bool menu = g.menuOpen.load();
            if (menu != s_menuWasOpen)
            {
                s_menuWasOpen = menu;
                pc->bShowMouseCursor = menu;
                pc->SetIgnoreLookInput(menu);
                pc->SetIgnoreMoveInput(menu);
            }

            // queued console commands from the ImGui console tab
            std::vector<std::wstring> cmds;
            {
                std::lock_guard<std::mutex> lk(g.mtx);
                cmds.swap(g.cmdQueue);
            }
            for (const std::wstring& c : cmds)
            {
                UKismetSystemLibrary::ExecuteConsoleCommand(world, FString(c.c_str()), pc);
                Log("exec: %ls", c.c_str());
            }

            // FOV
            if (g.fovEnabled.load())
            {
                const float want = g.fov.load();
                if (!s_fovWasOn)
                {
                    s_fovWasOn = true;
                    if (pc->PlayerCameraManager) s_fovBaseline = pc->PlayerCameraManager->GetFOVAngle();
                    s_fovLast = 0.0f;
                }
                if (want != s_fovLast || (g_tickCount % 30) == 0)
                {
                    pc->FOV(want);
                    s_fovLast = want;
                }
            }
            else if (s_fovWasOn)
            {
                s_fovWasOn = false;
                pc->FOV(s_fovBaseline);
            }

            // game speed
            if (g.gameSpeedEnabled.load())
            {
                const float want = g.gameSpeed.load();
                if (want != s_gsLast || (g_tickCount % 60) == 0)
                {
                    UGameplayStatics::SetGlobalTimeDilation(world, want);
                    s_gsLast = want;
                }
                s_gsWasOn = true;
            }
            else if (s_gsWasOn)
            {
                s_gsWasOn = false;
                s_gsLast  = 1.0f;
                UGameplayStatics::SetGlobalTimeDilation(world, 1.0f);
            }

            TickPlayerSpeed(ch);
            TickFly(world, pc, ch, dt);

            // aim + delete (aim refresh at ~10 Hz is plenty)
            static AActor* s_target = nullptr;
            if ((g_tickCount % 6) == 0 || g.reqDelete.load())
            {
                s_target = TraceAim(world, pc, ch);
                PublishTarget(s_target);
            }
            if (g.reqDelete.exchange(false))
            {
                DeleteTarget(s_target, ch);
                s_target = nullptr;
                PublishTarget(nullptr);
            }
            if (g.reqUndo.exchange(false))    { UndoHidden(false); PublishTarget(nullptr); }
            if (g.reqUndoAll.exchange(false)) { UndoHidden(true);  PublishTarget(nullptr); }
        }


        void SafeTick(double dt)
        {
#ifdef _MSC_VER
            DWORD code = 0;
            __try { GameTick(dt); }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                const int n = ++g_faults;
                Log("EXCEPTION 0x%08lX in GameTick (fault %d/5, last stage: %s)", code, n, g_stage);
            }
#else
            GameTick(dt);
#endif
        }

        void Hooked_ProcessEvent(const UObject* self, UFunction* fn, void* parms)
        {
            g_origProcessEvent(self, fn, parms);

            ++g_peCalls;
            if (t_inTick) return;
            if (GetCurrentThreadId() != g_gameThreadId) { ++g_wrongThread; return; }
            if (g_faults.load() >= 5) return;

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double since = double(now.QuadPart - g_lastTick.QuadPart) / double(g_qpcFreq.QuadPart);
            if (since < 0.004) return;

            const double dt = (g_lastTick.QuadPart == 0 || since > 0.1) ? 0.016 : since;
            g_lastTick = now;
            ++g_tickCount;

            t_inTick = true;
            SafeTick(dt);
            t_inTick = false;
        }


        // ---- stale-offset heal ------------------------------------------------------------
        // The SDK finds GObjects as GetModuleHandle(NULL) + Offsets::GObjects, inlined into several
        // functions. After a game update that offset points at garbage. We cannot edit those
        // constants at runtime, but we can make GetModuleHandle(NULL) lie to *our own module only*
        // so that fakeBase + Offsets::GObjects lands exactly on the live array.
        uintptr_t g_fakeBase  = 0;
        uintptr_t g_selfBegin = 0, g_selfEnd = 0;
        HMODULE (WINAPI* o_GetModuleHandleA)(LPCSTR)  = nullptr;
        HMODULE (WINAPI* o_GetModuleHandleW)(LPCWSTR) = nullptr;

        bool CallerIsUs(void* ret)
        {
            const uintptr_t r = reinterpret_cast<uintptr_t>(ret);
            return r >= g_selfBegin && r < g_selfEnd;
        }
        HMODULE WINAPI Hk_GetModuleHandleA(LPCSTR n)
        {
            if (!n && g_fakeBase && CallerIsUs(_ReturnAddress())) return reinterpret_cast<HMODULE>(g_fakeBase);
            return o_GetModuleHandleA(n);
        }
        HMODULE WINAPI Hk_GetModuleHandleW(LPCWSTR n)
        {
            if (!n && g_fakeBase && CallerIsUs(_ReturnAddress())) return reinterpret_cast<HMODULE>(g_fakeBase);
            return o_GetModuleHandleW(n);
        }

        bool InstallBaseHeal(uintptr_t fakeBase)
        {
            HMODULE self = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(&InstallBaseHeal), &self)) return false;
            auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(self);
            auto* nt  = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uintptr_t>(self) + dos->e_lfanew);
            g_selfBegin = reinterpret_cast<uintptr_t>(self);
            g_selfEnd   = g_selfBegin + nt->OptionalHeader.SizeOfImage;
            g_fakeBase  = fakeBase;

            HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            void* pa = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "GetModuleHandleA")) : nullptr;
            void* pw = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "GetModuleHandleW")) : nullptr;
            if (!pa || !pw) return false;
            if (MH_CreateHook(pa, reinterpret_cast<void*>(&Hk_GetModuleHandleA), reinterpret_cast<void**>(&o_GetModuleHandleA)) != MH_OK) return false;
            if (MH_CreateHook(pw, reinterpret_cast<void*>(&Hk_GetModuleHandleW), reinterpret_cast<void**>(&o_GetModuleHandleW)) != MH_OK) return false;
            return MH_EnableHook(pa) == MH_OK && MH_EnableHook(pw) == MH_OK;
        }

        BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
            char cls[64] = {};
            GetClassNameA(hwnd, cls, sizeof(cls));
            if (std::strcmp(cls, "UnrealWindow") != 0) return TRUE;
            *reinterpret_cast<HWND*>(out) = hwnd;
            return FALSE;
        }
    }

    unsigned BakedGObjects() { return static_cast<unsigned>(Offsets::GObjects); }

    bool Install()
    {
        QueryPerformanceFrequency(&g_qpcFreq);

        HWND game = nullptr;
        for (int i = 0; i < 600 && !game; ++i)
        {
            EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&game));
            if (!game) Sleep(100);
        }
        if (!game)
        {
            Log("UnrealWindow not found");
            return false;
        }
        g.gameHwnd.store(game);
        g_gameThreadId = GetWindowThreadProcessId(game, nullptr);

        const HMODULE   exe  = static_cast<HMODULE>(g_exeBase);
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);

        // 1) live GObjects: trust the SDK offset only if it validates, otherwise scan for the array
        Resolve::ObjArrayInfo arr{};
        const uintptr_t bakedAddr = base + static_cast<uintptr_t>(Offsets::GObjects);
        bool bakedOk = false;
        for (int i = 0; i < 300; ++i)                       // up to ~60 s while the engine boots
        {
            if (Resolve::ValidateObjArray(bakedAddr, &arr)) { bakedOk = true; break; }
            if (Resolve::ScanObjArray(exe, &arr)) break;
            Sleep(200);
        }
        if (!arr.addr)
        {
            Log("GObjects not found by SDK offset (base+0x%X) or by scan. Not hooking anything.", static_cast<unsigned>(Offsets::GObjects));
            return false;
        }
        Log("image base %p | GObjects live at base+0x%llX | num=%d chunks=%d | SDK offset 0x%X %s",
            reinterpret_cast<void*>(base), static_cast<unsigned long long>(arr.addr - base), arr.num, arr.chunks,
            static_cast<unsigned>(Offsets::GObjects), bakedOk ? "is valid" : "is STALE");

        if (!bakedOk)
        {
            if (InstallBaseHeal(arr.addr - static_cast<uintptr_t>(Offsets::GObjects)))
                Log("SDK GObjects offset healed at runtime. To make it permanent set Offsets::GObjects = 0x%llX in Basic.hpp.",
                    static_cast<unsigned long long>(arr.addr - base));
            else
                Log("Could not install the GObjects heal hook, SDK name/class lookups will misbehave.");
        }

        // 2) ProcessEvent: vote across live vtables, never hook an address we have not validated
        int votes = 0, scanned = 0;
        const uintptr_t pe = Resolve::VotePE(arr, static_cast<int>(Offsets::ProcessEventIdx), &votes, &scanned);
        const uintptr_t peBaked = base + static_cast<uintptr_t>(Offsets::ProcessEvent);
        Log("ProcessEvent: vtable idx %d, %d/%d votes -> %p (base+0x%llX) | SDK offset 0x%X -> %p",
            static_cast<int>(Offsets::ProcessEventIdx), votes, scanned, reinterpret_cast<void*>(pe),
            static_cast<unsigned long long>(pe ? pe - base : 0), static_cast<unsigned>(Offsets::ProcessEvent),
            reinterpret_cast<void*>(peBaked));

        if (!pe || scanned < 100 || votes * 2 < scanned)
        {
            Log("ProcessEvent could not be resolved with confidence (SDK/engine mismatch?). Not hooking.");
            return false;
        }
        if (pe != peBaked)
            Log("SDK ProcessEvent offset is stale. Live value is 0x%llX, using that.", static_cast<unsigned long long>(pe - base));

        void* target = reinterpret_cast<void*>(pe);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&Hooked_ProcessEvent),
                          reinterpret_cast<void**>(&g_origProcessEvent)) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            Log("ProcessEvent hook failed at %p", target);
            return false;
        }
        Log("ProcessEvent hooked at %p, game thread %lu", target, g_gameThreadId);

        std::thread([]
        {
            for (int i = 0; i < 40; ++i)
            {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                Log("heartbeat: PE calls=%llu wrongThread=%llu ticks=%llu faults=%d stage=%s",
                    g_peCalls.load(), g_wrongThread.load(), g_ticksRun.load(), g_faults.load(), g_stage);
                if (i == 4 && g_peCalls.load() == 0)
                    Log("WARNING: ProcessEvent never fired in 15 s. Wrong exe for this DLL, or the vtable index is wrong.");
            }
        }).detach();
        return true;
    }
}
