// DXGI Present hook with ImGui. Handles D3D11 and D3D12 at runtime.
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>

#include "MinHook.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "shared.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace Mod::Render
{
    namespace
    {
        using Present_t       = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        using ResizeBuffers_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        using ExecuteCL_t     = void    (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

        Present_t       o_Present       = nullptr;
        ResizeBuffers_t o_ResizeBuffers = nullptr;
        ExecuteCL_t     o_ExecuteCL     = nullptr;
        WNDPROC         o_WndProc       = nullptr;

        HWND  g_hwnd       = nullptr;
        bool  g_imguiReady = false;
        bool  g_isDx12     = false;

        // ---- D3D11 ----
        ID3D11Device*           d11_dev = nullptr;
        ID3D11DeviceContext*    d11_ctx = nullptr;
        ID3D11RenderTargetView* d11_rtv = nullptr;

        // ---- D3D12 ----
        constexpr UINT kMaxFrames = 8;
        ID3D12Device*              d12_dev      = nullptr;
        ID3D12CommandQueue*        d12_queue    = nullptr;   // captured from ExecuteCommandLists
        ID3D12DescriptorHeap*      d12_rtvHeap  = nullptr;
        ID3D12DescriptorHeap*      d12_srvHeap  = nullptr;
        ID3D12GraphicsCommandList* d12_list     = nullptr;
        ID3D12CommandAllocator*    d12_alloc[kMaxFrames]  = {};
        ID3D12Resource*            d12_back[kMaxFrames]   = {};
        UINT64                     d12_fenceVal[kMaxFrames] = {};
        ID3D12Fence*               d12_fence    = nullptr;
        HANDLE                     d12_event    = nullptr;
        UINT64                     d12_fenceCounter = 0;
        UINT                       d12_bufCount = 0;
        UINT                       d12_rtvInc   = 0;
        DXGI_FORMAT                d12_format   = DXGI_FORMAT_R8G8B8A8_UNORM;

        // hotkey edge state
        bool k_fly = false, k_del = false, k_undo = false;

        // ---------------------------------------------------------------------
        LRESULT CALLBACK Hooked_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_KEYDOWN && wp == VK_INSERT && !(lp & (1 << 30)))
            {
                g.menuOpen.store(!g.menuOpen.load());
                return 0;
            }

            if (g_imguiReady && g.menuOpen.load())
            {
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
                switch (msg)
                {
                case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
                case WM_INPUT: case WM_SETCURSOR:
                    return 0;
                default: break;
                }
            }
            return CallWindowProcW(o_WndProc, hwnd, msg, wp, lp);
        }

        void PollHotkeys()
        {
            if (GetForegroundWindow() != g_hwnd) return;
            auto edge = [](int vk, bool& prev) {
                const bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
                const bool pressed = d && !prev;
                prev = d;
                return pressed;
            };
            if (edge(VK_F6,     k_fly))  g.fly.store(!g.fly.load());
            if (edge(VK_DELETE, k_del))  g.reqDelete.store(true);
            if (edge(VK_F7,     k_undo)) g.reqUndo.store(true);
        }

        void BeginUiFrame()
        {
            PollHotkeys();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            ImGuiIO& io = ImGui::GetIO();
            io.MouseDrawCursor = g.menuOpen.load();
            g.uiWantsKeyboard.store(io.WantTextInput);
            if (g.menuOpen.load())
            {
                ClipCursor(nullptr);
                Menu::Draw();
            }
            ImGui::EndFrame();
            ImGui::Render();
        }

        void InitImGuiCommon()
        {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            ImGui::StyleColorsDark();
            ImGui_ImplWin32_Init(g_hwnd);
            o_WndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Hooked_WndProc)));
        }

        // ---------------------------------------------------------------------
        // D3D11 path
        // ---------------------------------------------------------------------
        void Dx11_CreateRtv(IDXGISwapChain* sc)
        {
            ID3D11Texture2D* back = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) && back)
            {
                d11_dev->CreateRenderTargetView(back, nullptr, &d11_rtv);
                back->Release();
            }
        }

        void Dx11_Frame(IDXGISwapChain* sc)
        {
            if (!g_imguiReady)
            {
                sc->GetDevice(IID_PPV_ARGS(&d11_dev));
                d11_dev->GetImmediateContext(&d11_ctx);
                DXGI_SWAP_CHAIN_DESC d{};
                sc->GetDesc(&d);
                g_hwnd = d.OutputWindow;
                InitImGuiCommon();
                ImGui_ImplDX11_Init(d11_dev, d11_ctx);
                g_imguiReady = true;
                Log("ImGui ready (D3D11)");
            }
            if (!d11_rtv) Dx11_CreateRtv(sc);

            ImGui_ImplDX11_NewFrame();
            BeginUiFrame();
            d11_ctx->OMSetRenderTargets(1, &d11_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        // ---------------------------------------------------------------------
        // D3D12 path
        // ---------------------------------------------------------------------
        void Dx12_WaitIdle()
        {
            if (!d12_queue || !d12_fence) return;
            const UINT64 v = ++d12_fenceCounter;
            if (SUCCEEDED(d12_queue->Signal(d12_fence, v)) && d12_fence->GetCompletedValue() < v)
            {
                d12_fence->SetEventOnCompletion(v, d12_event);
                WaitForSingleObject(d12_event, 2000);
            }
        }

        void Dx12_ReleaseBackBuffers()
        {
            for (UINT i = 0; i < kMaxFrames; ++i)
                if (d12_back[i]) { d12_back[i]->Release(); d12_back[i] = nullptr; }
        }

        void Dx12_CreateRtvs(IDXGISwapChain3* sc)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = d12_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < d12_bufCount; ++i)
            {
                if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&d12_back[i]))))
                    d12_dev->CreateRenderTargetView(d12_back[i], nullptr, h);
                h.ptr += d12_rtvInc;
            }
        }

        bool Dx12_Init(IDXGISwapChain3* sc)
        {
            DXGI_SWAP_CHAIN_DESC d{};
            sc->GetDesc(&d);
            g_hwnd       = d.OutputWindow;
            d12_bufCount = d.BufferCount < kMaxFrames ? d.BufferCount : kMaxFrames;
            d12_format   = d.BufferDesc.Format;

            if (FAILED(sc->GetDevice(IID_PPV_ARGS(&d12_dev)))) return false;

            D3D12_DESCRIPTOR_HEAP_DESC rtv{};
            rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtv.NumDescriptors = d12_bufCount;
            if (FAILED(d12_dev->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&d12_rtvHeap)))) return false;
            d12_rtvInc = d12_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

            D3D12_DESCRIPTOR_HEAP_DESC srv{};
            srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srv.NumDescriptors = 1;
            srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(d12_dev->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&d12_srvHeap)))) return false;

            for (UINT i = 0; i < d12_bufCount; ++i)
                if (FAILED(d12_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d12_alloc[i]))))
                    return false;

            if (FAILED(d12_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d12_alloc[0], nullptr, IID_PPV_ARGS(&d12_list))))
                return false;
            d12_list->Close();

            if (FAILED(d12_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d12_fence)))) return false;
            d12_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

            Dx12_CreateRtvs(sc);

            InitImGuiCommon();

            ImGui_ImplDX12_InitInfo info;
            info.Device            = d12_dev;
            info.CommandQueue      = d12_queue;
            info.NumFramesInFlight = static_cast<int>(d12_bufCount);
            info.RTVFormat         = d12_format;
            info.DSVFormat         = DXGI_FORMAT_UNKNOWN;
            info.SrvDescriptorHeap = d12_srvHeap;
            info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
            {
                *cpu = i->SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
                *gpu = i->SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
            };
            info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
            ImGui_ImplDX12_Init(&info);
            return true;
        }

        void Dx12_Frame(IDXGISwapChain3* sc)
        {
            if (!d12_queue)
            {
                static bool warned = false;
                if (!warned) { warned = true; Log("D3D12: waiting for the direct command queue (ExecuteCommandLists not seen yet)"); }
                return;
            }
            if (!g_imguiReady)
            {
                if (!Dx12_Init(sc)) { Log("D3D12 init failed"); return; }
                g_imguiReady = true;
                Log("ImGui ready (D3D12)");
            }

            ImGui_ImplDX12_NewFrame();
            BeginUiFrame();

            const UINT idx = sc->GetCurrentBackBufferIndex() % d12_bufCount;
            if (d12_fence->GetCompletedValue() < d12_fenceVal[idx])
            {
                d12_fence->SetEventOnCompletion(d12_fenceVal[idx], d12_event);
                WaitForSingleObject(d12_event, 2000);
            }

            d12_alloc[idx]->Reset();
            d12_list->Reset(d12_alloc[idx], nullptr);

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = d12_back[idx];
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            d12_list->ResourceBarrier(1, &b);

            D3D12_CPU_DESCRIPTOR_HANDLE rtv = d12_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += static_cast<SIZE_T>(idx) * d12_rtvInc;
            d12_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            d12_list->SetDescriptorHeaps(1, &d12_srvHeap);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), d12_list);

            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            d12_list->ResourceBarrier(1, &b);
            d12_list->Close();

            ID3D12CommandList* lists[] = { d12_list };
            d12_queue->ExecuteCommandLists(1, lists);
            d12_queue->Signal(d12_fence, d12_fenceVal[idx] = ++d12_fenceCounter);
        }

        // ---------------------------------------------------------------------
        void STDMETHODCALLTYPE Hooked_ExecuteCL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l)
        {
            if (!d12_queue)
            {
                const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
                if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) d12_queue = q;
            }
            o_ExecuteCL(q, n, l);
        }

        HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
        {
            static thread_local bool busy = false;
            static bool firstCall = true;
            if (firstCall) { firstCall = false; Log("Present hook is being called"); }
            if (!busy)
            {
                busy = true;
                ID3D12Device* dev12 = nullptr;
                if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&dev12))) && dev12)
                {
                    dev12->Release();
                    g_isDx12 = true;
                    IDXGISwapChain3* sc3 = nullptr;
                    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3)
                    {
                        Dx12_Frame(sc3);
                        sc3->Release();
                    }
                }
                else
                {
                    g_isDx12 = false;
                    Dx11_Frame(sc);
                }
                busy = false;
            }
            return o_Present(sc, sync, flags);
        }

        HRESULT STDMETHODCALLTYPE Hooked_ResizeBuffers(IDXGISwapChain* sc, UINT cnt, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
        {
            if (g_imguiReady)
            {
                if (g_isDx12)
                {
                    Dx12_WaitIdle();
                    Dx12_ReleaseBackBuffers();
                    ImGui_ImplDX12_InvalidateDeviceObjects();
                }
                else
                {
                    if (d11_rtv) { d11_rtv->Release(); d11_rtv = nullptr; }
                    ImGui_ImplDX11_InvalidateDeviceObjects();
                }
            }

            const HRESULT hr = o_ResizeBuffers(sc, cnt, w, h, fmt, flags);

            if (g_imguiReady && SUCCEEDED(hr))
            {
                if (g_isDx12)
                {
                    ImGui_ImplDX12_CreateDeviceObjects();
                    IDXGISwapChain3* sc3 = nullptr;
                    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3)
                    {
                        DXGI_SWAP_CHAIN_DESC d{};
                        sc->GetDesc(&d);
                        d12_bufCount = d.BufferCount < kMaxFrames ? d.BufferCount : kMaxFrames;
                        Dx12_CreateRtvs(sc3);
                        sc3->Release();
                    }
                }
                else
                {
                    ImGui_ImplDX11_CreateDeviceObjects();
                }
            }
            return hr;
        }

        // Dummy-window trick: read vtable pointers once, then hook the shared implementations.
        LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

        bool ResolveVtables(void*& present, void*& resize, void*& executeCl)
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc   = DummyProc;
            wc.hInstance     = static_cast<HINSTANCE>(g_exeBase);   // not GetModuleHandle(NULL): the SDK heal hook may answer that
            wc.lpszClassName = L"PumpppttPlaytimeDummy";
            RegisterClassExW(&wc);
            HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
            if (!hwnd) return false;

            bool ok = false;
            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferCount       = 2;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow      = hwnd;
            sd.SampleDesc.Count  = 1;
            sd.Windowed          = TRUE;
            sd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;

            // D3D12 dummy: gives swapchain vtable and ExecuteCommandLists
            HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
            if (d3d12)
            {
                auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
                ID3D12Device* dev = nullptr;
                if (create && SUCCEEDED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
                {
                    D3D12_COMMAND_QUEUE_DESC qd{};
                    ID3D12CommandQueue* q = nullptr;
                    IDXGIFactory4*      f = nullptr;
                    IDXGISwapChain*     s = nullptr;
                    if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) &&
                        SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) &&
                        SUCCEEDED(f->CreateSwapChain(q, &sd, &s)))
                    {
                        void** svt = *reinterpret_cast<void***>(s);
                        void** qvt = *reinterpret_cast<void***>(q);
                        present = svt[8]; resize = svt[13]; executeCl = qvt[10];
                        ok = true;
                    }
                    if (s) s->Release();
                    if (f) f->Release();
                    if (q) q->Release();
                    dev->Release();
                }
            }

            // D3D11 fallback
            if (!ok)
            {
                sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
                ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* s = nullptr;
                D3D_FEATURE_LEVEL fl;
                if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                            D3D11_SDK_VERSION, &sd, &s, &dev, &fl, &ctx)))
                {
                    void** svt = *reinterpret_cast<void***>(s);
                    present = svt[8]; resize = svt[13]; executeCl = nullptr;
                    ok = true;
                }
                if (s) s->Release();
                if (ctx) ctx->Release();
                if (dev) dev->Release();
            }

            DestroyWindow(hwnd);
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
            return ok;
        }
    }

    bool Install()
    {
        // Wait for the engine's renderer module so the dummy device matches the game's API.
        for (int i = 0; i < 600 && !(GetModuleHandleW(L"d3d12.dll") || GetModuleHandleW(L"d3d11.dll")); ++i) Sleep(100);
        Sleep(2000);

        void *present = nullptr, *resize = nullptr, *execCl = nullptr;
        if (!ResolveVtables(present, resize, execCl))
        {
            Log("Could not resolve DXGI vtables");
            return false;
        }

        if (MH_CreateHook(present, reinterpret_cast<void*>(&Hooked_Present), reinterpret_cast<void**>(&o_Present)) != MH_OK ||
            MH_CreateHook(resize,  reinterpret_cast<void*>(&Hooked_ResizeBuffers), reinterpret_cast<void**>(&o_ResizeBuffers)) != MH_OK)
            return false;
        if (execCl &&
            MH_CreateHook(execCl, reinterpret_cast<void*>(&Hooked_ExecuteCL), reinterpret_cast<void**>(&o_ExecuteCL)) != MH_OK)
            return false;

        const bool ok = MH_EnableHook(present) == MH_OK && MH_EnableHook(resize) == MH_OK &&
                        (!execCl || MH_EnableHook(execCl) == MH_OK);
        Log("Present hooked at %p (%s)", present, execCl ? "D3D12 capable" : "D3D11 only");
        return ok;
    }
}
