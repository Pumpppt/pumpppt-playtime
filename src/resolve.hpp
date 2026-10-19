// SDK-free runtime resolution. Nothing in here depends on a Dumper-7 dump, so it keeps
// working after a game update. Layout facts used (identical in UE 4.27 and UE 5.x):
//   FChunkedFixedUObjectArray: Objects(+0x00) Prealloc(+0x08) Max(+0x10) Num(+0x14)
//                              MaxChunks(+0x18) NumChunks(+0x1C), 65536 items per chunk,
//                              FUObjectItem is 0x18 bytes with the UObject* first.
#pragma once
#include <Windows.h>
#include <cstdint>

namespace Mod::Resolve
{
    struct ObjArrayInfo
    {
        uintptr_t addr   = 0;   // address of the array header (what the SDK calls TUObjectArray)
        int       num    = 0;
        int       max    = 0;
        int       chunks = 0;
    };

    bool Rd(uintptr_t addr, void* out, size_t bytes);              // fault-safe read of this process
    bool IsExecutable(uintptr_t addr);                             // committed + executable page
    bool ValidateObjArray(uintptr_t addr, ObjArrayInfo* out);      // does a live, sane GObjects live here?
    bool ScanObjArray(HMODULE image, ObjArrayInfo* out);           // hunt it in the exe's writable sections
    uintptr_t VotePE(const ObjArrayInfo& arr, int vtableIdx, int* votes, int* scanned);
    const char* EngineTag(HMODULE image);                          // "4.27", "5.1", "5.3", "5.6" or ""
}
