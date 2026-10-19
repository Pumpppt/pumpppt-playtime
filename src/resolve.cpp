#include "resolve.hpp"
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#pragma comment(lib, "version.lib")

namespace Mod::Resolve
{
    namespace
    {
        constexpr int kPerChunk = 65536;
        constexpr size_t kItemSize = 0x18;

        struct Sec { uintptr_t begin; size_t size; DWORD flags; };

        std::vector<Sec> Sections(HMODULE image)
        {
            std::vector<Sec> v;
            const uintptr_t base = reinterpret_cast<uintptr_t>(image);
            IMAGE_DOS_HEADER dos{};
            if (!Rd(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return v;
            IMAGE_NT_HEADERS64 nt{};
            if (!Rd(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return v;
            const uintptr_t first = base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
            for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i)
            {
                IMAGE_SECTION_HEADER s{};
                if (!Rd(first + i * sizeof(s), &s, sizeof(s))) break;
                v.push_back({ base + s.VirtualAddress, s.Misc.VirtualSize, s.Characteristics });
            }
            return v;
        }

        bool HeaderPlausible(const uint8_t* h)
        {
            const uintptr_t objs = *reinterpret_cast<const uintptr_t*>(h);
            const int max = *reinterpret_cast<const int*>(h + 0x10);
            const int num = *reinterpret_cast<const int*>(h + 0x14);
            const int mch = *reinterpret_cast<const int*>(h + 0x18);
            const int nch = *reinterpret_cast<const int*>(h + 0x1C);
            if (num < 1000 || num > 8000000) return false;
            if (max < num || max > 16000000) return false;
            if (mch < 1 || mch > 256 || (max + kPerChunk - 1) / kPerChunk != mch) return false;
            if (nch < 1 || nch > mch || (num + kPerChunk - 1) / kPerChunk > nch) return false;
            if (objs < 0x10000 || objs > 0x7FFFFFFFFFFFull || (objs & 7)) return false;
            return true;
        }
    }

    bool Rd(uintptr_t addr, void* out, size_t bytes)
    {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, bytes, &got) && got == bytes;
    }

    bool IsExecutable(uintptr_t addr)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (mbi.Protect & kExec) != 0;
    }

    bool ValidateObjArray(uintptr_t addr, ObjArrayInfo* out)
    {
        uint8_t h[0x20];
        if (!addr || !Rd(addr, h, sizeof(h)) || !HeaderPlausible(h)) return false;

        const uintptr_t objs = *reinterpret_cast<uintptr_t*>(h);
        const int num = *reinterpret_cast<int*>(h + 0x14);
        const int max = *reinterpret_cast<int*>(h + 0x10);
        const int nch = *reinterpret_cast<int*>(h + 0x1C);

        std::vector<uintptr_t> chunks(static_cast<size_t>(nch));
        if (!Rd(objs, chunks.data(), chunks.size() * sizeof(uintptr_t))) return false;
        const int used = (num + kPerChunk - 1) / kPerChunk;
        for (int i = 0; i < used; ++i)
            if (chunks[i] < 0x10000 || (chunks[i] & 7)) return false;

        // Sample the first items of chunk 0: real UObjects have a vtable in readable memory
        // whose first entry points at executable code.
        std::vector<uint8_t> items(512 * kItemSize);
        if (!Rd(chunks[0], items.data(), items.size())) return false;
        int good = 0, bad = 0;
        for (int i = 0; i < 512; ++i)
        {
            const uintptr_t obj = *reinterpret_cast<uintptr_t*>(&items[i * kItemSize]);
            if (!obj) continue;
            uintptr_t vt = 0, fn0 = 0;
            if (Rd(obj, &vt, 8) && vt && Rd(vt, &fn0, 8) && IsExecutable(fn0)) ++good; else ++bad;
        }
        if (good < 32 || bad * 10 > good) return false;

        if (out) { out->addr = addr; out->num = num; out->max = max; out->chunks = nch; }
        return true;
    }

    bool ScanObjArray(HMODULE image, ObjArrayInfo* out)
    {
        ObjArrayInfo best{};
        constexpr size_t kBlock = 1u << 20;
        std::vector<uint8_t> buf(kBlock + 0x20);

        for (const Sec& s : Sections(image))
        {
            if (!(s.flags & IMAGE_SCN_MEM_WRITE) || (s.flags & IMAGE_SCN_MEM_EXECUTE) || s.size < 0x20) continue;
            for (size_t off = 0; off < s.size; off += kBlock)
            {
                const size_t want = (std::min)(kBlock + 0x20, s.size - off);
                if (want < 0x20) break;
                if (!Rd(s.begin + off, buf.data(), want)) continue;
                for (size_t p = 0; p + 0x20 <= want; p += 8)
                {
                    if (!HeaderPlausible(&buf[p])) continue;
                    ObjArrayInfo cand{};
                    if (ValidateObjArray(s.begin + off + p, &cand) && cand.num > best.num) best = cand;
                }
            }
        }
        if (!best.addr) return false;
        if (out) *out = best;
        return true;
    }

    uintptr_t VotePE(const ObjArrayInfo& arr, int vtableIdx, int* votes, int* scanned)
    {
        *votes = 0; *scanned = 0;
        uintptr_t objs = 0;
        if (!Rd(arr.addr, &objs, 8)) return 0;
        std::vector<uintptr_t> chunks(static_cast<size_t>(arr.chunks));
        if (!Rd(objs, chunks.data(), chunks.size() * sizeof(uintptr_t))) return 0;

        struct Cand { uintptr_t fn; int n; };
        std::vector<Cand> cands;
        int seen = 0;
        for (int i = 0; i < arr.num && seen < 6000; ++i)
        {
            const uintptr_t chunk = chunks[static_cast<size_t>(i / kPerChunk)];
            if (!chunk) continue;
            uintptr_t obj = 0, vt = 0, fn = 0;
            if (!Rd(chunk + static_cast<uintptr_t>(i % kPerChunk) * kItemSize, &obj, 8) || !obj) continue;
            if (!Rd(obj, &vt, 8) || !vt) continue;
            if (!Rd(vt + static_cast<uintptr_t>(vtableIdx) * 8, &fn, 8) || !fn) continue;
            ++seen;
            auto it = std::find_if(cands.begin(), cands.end(), [fn](const Cand& c) { return c.fn == fn; });
            if (it != cands.end()) ++it->n; else cands.push_back({ fn, 1 });
        }
        *scanned = seen;
        if (cands.empty()) return 0;
        const Cand best = *std::max_element(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.n < b.n; });
        *votes = best.n;
        return IsExecutable(best.fn) ? best.fn : 0;
    }

    const char* EngineTag(HMODULE image)
    {
        static const char* kTags[] = { "4.27", "5.0", "5.1", "5.2", "5.3", "5.4", "5.5", "5.6" };

        // 1) branch string baked into the exe, e.g. "++UE4+Release-4.27", ASCII or UTF-16
        std::vector<std::string> ascii, wide;
        for (const char* t : kTags)
        {
            std::string a = std::string("+Release-") + t;
            ascii.push_back(a);
            std::string w;
            for (char c : a) { w.push_back(c); w.push_back('\0'); }
            wide.push_back(w);
        }
        std::vector<uint8_t> buf((4u << 20) + 64);
        for (const Sec& s : Sections(image))
        {
            if ((s.flags & IMAGE_SCN_MEM_EXECUTE) || (s.flags & IMAGE_SCN_MEM_WRITE)) continue;   // .rdata-like only
            for (size_t off = 0; off < s.size; off += (4u << 20))
            {
                const size_t want = (std::min)(buf.size(), s.size - off);
                if (!Rd(s.begin + off, buf.data(), want)) continue;
                const std::string_view hay(reinterpret_cast<const char*>(buf.data()), want);
                for (size_t k = 0; k < ascii.size(); ++k)
                    if (hay.find(ascii[k]) != std::string_view::npos || hay.find(wide[k]) != std::string_view::npos)
                        return kTags[k];
            }
        }

        // 2) exe version resource, only trusted when it actually looks like an engine version
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(image, path, MAX_PATH);
        DWORD dummy = 0;
        const DWORD sz = GetFileVersionInfoSizeW(path, &dummy);
        if (sz)
        {
            std::vector<uint8_t> data(sz);
            VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
            if (GetFileVersionInfoW(path, 0, sz, data.data()) && VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) && ffi)
            {
                const int maj = HIWORD(ffi->dwFileVersionMS), min = LOWORD(ffi->dwFileVersionMS);
                for (const char* t : kTags)
                    if (maj == t[0] - '0' && min == std::atoi(t + 2)) return t;
            }
        }
        return "";
    }
}
