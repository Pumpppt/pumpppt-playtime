#include <Windows.h>
#include <string>
#include "imgui.h"
#include "shared.hpp"

namespace Mod::Menu
{
    namespace
    {
        template <typename T>
        bool SliderAtomic(const char* label, std::atomic<T>& v, T lo, T hi, const char* fmt)
        {
            T tmp = v.load();
            const bool changed = ImGui::SliderFloat(label, &tmp, lo, hi, fmt);
            if (changed) v.store(tmp);
            return changed;
        }

        bool CheckAtomic(const char* label, std::atomic<bool>& v)
        {
            bool tmp = v.load();
            const bool changed = ImGui::Checkbox(label, &tmp);
            if (changed) v.store(tmp);
            return changed;
        }

        std::wstring Widen(const char* s)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
            if (n <= 1) return {};
            std::wstring w(static_cast<size_t>(n - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
            return w;
        }
    }

    void Draw()
    {
        ImGui::SetNextWindowSize(ImVec2(520, 440), ImGuiCond_FirstUseEver);
        ImGui::Begin("pumpppt-playtime");

        ImGui::Text("Chapter SDK: %d    World: ", MOD_CHAPTER);
        ImGui::SameLine();
        {
            std::lock_guard<std::mutex> lk(g.mtx);
            ImGui::TextUnformatted(g.worldName.c_str());
        }
        ImGui::Separator();

        if (ImGui::BeginTabBar("tabs"))
        {
            if (ImGui::BeginTabItem("Player"))
            {
                CheckAtomic("Fly / noclip (F6)", g.fly);
                CheckAtomic("Noclip while flying", g.flyNoclip);
                SliderAtomic("Fly speed", g.flySpeed, 100.0f, 10000.0f, "%.0f");
                SliderAtomic("Shift boost", g.flyBoost, 1.0f, 10.0f, "x%.1f");
                ImGui::TextDisabled("WASD move along camera, Space up, Ctrl down, Shift boost");
                ImGui::Separator();
                CheckAtomic("Player speed", g.playerSpeedEnabled);
                SliderAtomic("Multiplier", g.playerSpeed, 0.1f, 10.0f, "x%.2f");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Camera"))
            {
                CheckAtomic("Override FOV", g.fovEnabled);
                SliderAtomic("FOV", g.fov, 30.0f, 170.0f, "%.0f");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("World"))
            {
                CheckAtomic("Game speed", g.gameSpeedEnabled);
                SliderAtomic("Time dilation", g.gameSpeed, 0.01f, 10.0f, "x%.2f");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Delete"))
            {
                {
                    std::lock_guard<std::mutex> lk(g.mtx);
                    ImGui::Text("Aimed actor : %s", g.targetName.c_str());
                    ImGui::Text("Class       : %s", g.targetClass.c_str());
                    ImGui::Text("Hidden      : %d", g.hiddenCount);
                }
                if (ImGui::Button("Delete aimed (DEL)")) g.reqDelete.store(true);
                ImGui::SameLine();
                if (ImGui::Button("Undo last (F7)")) g.reqUndo.store(true);
                ImGui::SameLine();
                if (ImGui::Button("Undo all")) g.reqUndoAll.store(true);
                CheckAtomic("Hard destroy (cannot undo)", g.hardDestroy);
                int ch = g.traceChannel.load();
                if (ImGui::SliderInt("Trace channel", &ch, 0, 20)) g.traceChannel.store(ch);
                SliderAtomic("Trace distance", g.traceDistance, 500.0f, 100000.0f, "%.0f");
                ImGui::TextDisabled("Channel 0 = Visibility. Try 1..N if a mesh is not picked up.");
                ImGui::TextDisabled("Deleting removes the whole owning actor, not only the mesh.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Console"))
            {
                CheckAtomic("Unlock in-game console (~ / F2)", g.unlockConsole);
                {
                    std::lock_guard<std::mutex> lk(g.mtx);
                    ImGui::Text("Status: %s", g.consoleStatus.c_str());
                }
                static char input[256] = {};
                ImGui::SetNextItemWidth(-90.0f);
                const bool enter = ImGui::InputText("##cmd", input, sizeof(input), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("Run") || enter) && input[0])
                {
                    std::lock_guard<std::mutex> lk(g.mtx);
                    g.cmdQueue.push_back(Widen(input));
                    input[0] = '\0';
                    ImGui::SetKeyboardFocusHere(-1);
                }
                ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
                {
                    std::lock_guard<std::mutex> lk(g.mtx);
                    for (const std::string& line : g.logLines) ImGui::TextUnformatted(line.c_str());
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }
}
