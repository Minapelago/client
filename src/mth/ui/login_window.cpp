#include "mth/ui/login_window.hpp"

#include <cstdio>

#include <imgui.h>

#include "mth/app/app.hpp"
#include "mth/core/ap/ap_state.hpp" // ConnectionPhase

namespace mth
{

LoginWindow::LoginWindow(App &app) : app_(app)
{
}

// Deferred out of the ctor: the overlay is built during App construction, before App can answer. Fills only empty fields, so it never clobbers input.
void LoginWindow::prefill_once()
{
    if (prefilled_)
        return;
    prefilled_ = true;

    const SavedLogin saved = app_.saved_login();
    if (server_[0] == '\0')
        std::snprintf(server_.data(), server_.size(), "%s", saved.server.c_str());
    if (slot_[0] == '\0')
        std::snprintf(slot_.data(), slot_.size(), "%s", saved.slot.c_str());
}

void LoginWindow::draw(bool login_open)
{
    if (!login_open)
        return;

    prefill_once();

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 29.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Archipelago Connection"))
    {
        ImGui::End();
        return;
    }

    ImGui::InputText("Server", server_.data(), server_.size()); // host:port
    ImGui::InputText("Slot", slot_.data(), slot_.size());
    ImGui::InputText("Password", password_.data(), password_.size(), ImGuiInputTextFlags_Password);

    const ConnectionStatus status = app_.connection_status();
    const GateStatus gate = app_.gate_.status();
    const bool gate_blocks = should_refuse_connect(gate.enforcing, gate.verdict);
    switch (status.phase)
    {
    case ConnectionPhase::Connecting:
        ImGui::BeginDisabled();
        ImGui::Button("Connecting...");
        ImGui::EndDisabled();
        break;
    case ConnectionPhase::Connected:
        if (ImGui::Button("Disconnect"))
            app_.disconnect();
        break;
    case ConnectionPhase::Disconnected:
    case ConnectionPhase::Error:
        // App refuses the call anyway, but a gate refusal never reaches ApState as an Error phase, so the disabled button is what shows it.
        ImGui::BeginDisabled(gate_blocks);
        if (ImGui::Button("Connect"))
            app_.connect(server_.data(), slot_.data(), password_.data());
        ImGui::EndDisabled();
        break;
    }

    ImGui::Separator();
    switch (status.phase)
    {
    case ConnectionPhase::Disconnected:
        ImGui::TextUnformatted("Disconnected");
        break;
    case ConnectionPhase::Connecting:
        ImGui::TextUnformatted("Connecting...");
        break;
    case ConnectionPhase::Connected:
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Connected");
        break;
    case ConnectionPhase::Error:
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", status.detail.c_str());
        break;
    }

    if (gate_blocks)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Archipelago disabled: %s", gate.reason.c_str());

    ImGui::End();
}

} // namespace mth
