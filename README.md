# pumpppt-playtime

Single-player trainer for Poppy Playtime chapters 1 to 5. **One DLL for every chapter**: `pumpppt-playtime.dll`.

## How the single DLL works

Each chapter still compiles against its own Dumper-7 SDK (UE 4.27 and UE 5.x have different class layouts), but those
five modules are embedded inside `pumpppt-playtime.dll` as resources. On injection the loader unpacks them to
`%TEMP%\pumpppt-playtime\`, picks the one that fits the running game, unloads the rest and starts it.

Chapter detection, in order:
1. `PUMPPPT_CHAPTER=1..5` environment variable, or a file `pumpppt-playtime.chapter` (one digit) next to the DLL.
2. Engine branch string found inside the game exe (`++UE4+Release-4.27`, `++UE5+Release-5.3`, ...), falling back to the exe's file version.
3. Whether a module's SDK `GObjects` offset validates against the running exe.

Ch1 and ch2 are both 4.27.2, so the engine tag cannot tell them apart. They share engine class layouts and the mod only touches
engine classes, so either module works for both.

## Why ch1 / ch2 failed (the log)

```
GObjects count 69083556          <- not an object count, the SDK offset reads garbage
ProcessEvent hooked at base+0x1442A40, PE calls=0   <- hooked code that never runs
```
The baked offsets (ch1 `GObjects = 0x4BE5CD0`, ch2 `0x49396D0`) do not match the exe that was running, so everything that
depends on them was dead. This is a stale-SDK problem, not a UE4 problem.

What changed:
- `GObjects` is validated (chunked-array layout, sane counts, real vtables). If the SDK offset fails, the live array is found
  by scanning the exe's writable sections.
- The SDK computes `GetModuleHandle(NULL) + Offsets::GObjects` inline. When the offset is stale, a hook makes that call return
  a shifted base **for this module only**, so the SDK lands on the live array without regenerating anything.
- `ProcessEvent` is taken from a vote across live vtables (`Offsets::ProcessEventIdx`), validated as executable, and only hooked
  with majority agreement. The old code hooked the stale address blindly.
- The log prints the correct live offsets. To make the fix permanent, put them in each SDK's `Basic.hpp` (`Offsets::GObjects`,
  `Offsets::ProcessEvent`) or re-dump with Dumper-7.

## Build (Windows, Visual Studio 2022+, CMake 3.20+)

```
cmake -S . -B build -G "Ninja" -DPOPPY_SDK_ROOT=C:/path/to/SDKS
cmake --build build --config Release
```
Output: `build/pumpppt-playtime.dll` (inject this one). Chapters whose SDK folder is missing are skipped with a warning.
`build/modules/chN.dll` are intermediate files; do not inject them.

## Controls

| Key | Action |
|---|---|
| INSERT | Toggle menu |
| F6 | Fly / noclip (WASD, Space up, Ctrl down, Shift boost) |
| DELETE | Delete the actor under the crosshair |
| F7 | Undo last delete |
| ~ or F2 | In-game console (after unlock) |

## Known limits

- Single-player only. Do not use online.
- If `ProcessEvent` cannot be resolved with confidence the mod refuses to hook and says so in the console.
- No unload path. Restart the game to remove the DLL.
