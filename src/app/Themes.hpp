#pragma once

#include <SFML/Graphics/Color.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdint>

namespace weasel
{
    enum class UiTheme
    {
        MidnightBlue,
        Ember,
        Frost,
        Windows,
        AdobePremiere
    };

    inline void ApplyTheme(UiTheme theme)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle{};
        if (theme == UiTheme::Frost || theme == UiTheme::Windows)
        {
            ImGui::StyleColorsLight(&style);
        }
        else
        {
            ImGui::StyleColorsDark(&style);
        }

        const bool adobePremiere = theme == UiTheme::AdobePremiere;
        style.WindowRounding = adobePremiere ? 0.0f : 3.0f;
        style.ChildRounding = adobePremiere ? 0.0f : 2.0f;
        style.FrameRounding = adobePremiere ? 2.0f : 3.0f;
        style.GrabRounding = adobePremiere ? 2.0f : 3.0f;
        style.ScrollbarRounding = adobePremiere ? 0.0f : 3.0f;
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = theme == UiTheme::Windows ? 1.0f : 0.0f;
        style.WindowPadding = adobePremiere ? ImVec2(8.0f, 7.0f) : ImVec2(10.0f, 9.0f);
        style.ItemSpacing = adobePremiere ? ImVec2(6.0f, 5.0f) : ImVec2(7.0f, 6.0f);
        if (adobePremiere)
        {
            style.PopupRounding = 0.0f;
            style.PopupBorderSize = 1.0f;
            style.TabRounding = 0.0f;
        }

        ImVec4* colours = style.Colors;
        switch (theme)
        {
        case UiTheme::MidnightBlue:
            colours[ImGuiCol_Text] = ImVec4(0.89f, 0.91f, 0.94f, 1.0f);
            colours[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.52f, 0.58f, 1.0f);
            colours[ImGuiCol_WindowBg] = ImVec4(0.085f, 0.095f, 0.115f, 1.0f);
            colours[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.083f, 0.10f, 1.0f);
            colours[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.135f, 1.0f);
            colours[ImGuiCol_Border] = ImVec4(0.20f, 0.23f, 0.29f, 1.0f);
            colours[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.20f, 1.0f);
            colours[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.23f, 0.30f, 1.0f);
            colours[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.28f, 0.38f, 1.0f);
            colours[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.11f, 0.14f, 1.0f);
            colours[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.11f, 0.14f, 1.0f);
            colours[ImGuiCol_MenuBarBg] = ImVec4(0.105f, 0.115f, 0.14f, 1.0f);
            colours[ImGuiCol_Button] = ImVec4(0.16f, 0.28f, 0.45f, 1.0f);
            colours[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.39f, 0.63f, 1.0f);
            colours[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.48f, 0.70f, 1.0f);
            colours[ImGuiCol_Header] = ImVec4(0.16f, 0.29f, 0.48f, 0.75f);
            colours[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.40f, 0.65f, 0.9f);
            colours[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.47f, 0.76f, 1.0f);
            colours[ImGuiCol_Separator] = ImVec4(0.22f, 0.25f, 0.31f, 1.0f);
            colours[ImGuiCol_ResizeGrip] = ImVec4(0.23f, 0.42f, 0.68f, 0.35f);
            colours[ImGuiCol_ScrollbarBg] = ImVec4(0.06f, 0.07f, 0.09f, 1.0f);
            colours[ImGuiCol_ScrollbarGrab] = ImVec4(0.27f, 0.31f, 0.38f, 1.0f);
            break;

        case UiTheme::Ember:
            colours[ImGuiCol_Text] = ImVec4(0.95f, 0.90f, 0.84f, 1.0f);
            colours[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.49f, 0.43f, 1.0f);
            colours[ImGuiCol_WindowBg] = ImVec4(0.115f, 0.085f, 0.070f, 1.0f);
            colours[ImGuiCol_ChildBg] = ImVec4(0.090f, 0.065f, 0.055f, 1.0f);
            colours[ImGuiCol_PopupBg] = ImVec4(0.135f, 0.100f, 0.080f, 1.0f);
            colours[ImGuiCol_Border] = ImVec4(0.35f, 0.23f, 0.17f, 1.0f);
            colours[ImGuiCol_FrameBg] = ImVec4(0.22f, 0.13f, 0.09f, 1.0f);
            colours[ImGuiCol_FrameBgHovered] = ImVec4(0.34f, 0.20f, 0.10f, 1.0f);
            colours[ImGuiCol_FrameBgActive] = ImVec4(0.48f, 0.25f, 0.10f, 1.0f);
            colours[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.08f, 0.065f, 1.0f);
            colours[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.08f, 0.065f, 1.0f);
            colours[ImGuiCol_MenuBarBg] = ImVec4(0.145f, 0.095f, 0.070f, 1.0f);
            colours[ImGuiCol_Button] = ImVec4(0.44f, 0.20f, 0.08f, 1.0f);
            colours[ImGuiCol_ButtonHovered] = ImVec4(0.66f, 0.31f, 0.10f, 1.0f);
            colours[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.42f, 0.12f, 1.0f);
            colours[ImGuiCol_Header] = ImVec4(0.42f, 0.20f, 0.08f, 0.75f);
            colours[ImGuiCol_HeaderHovered] = ImVec4(0.64f, 0.31f, 0.10f, 0.90f);
            colours[ImGuiCol_HeaderActive] = ImVec4(0.78f, 0.40f, 0.12f, 1.0f);
            colours[ImGuiCol_Separator] = ImVec4(0.35f, 0.23f, 0.17f, 1.0f);
            colours[ImGuiCol_ResizeGrip] = ImVec4(0.75f, 0.36f, 0.10f, 0.35f);
            colours[ImGuiCol_ScrollbarBg] = ImVec4(0.070f, 0.050f, 0.045f, 1.0f);
            colours[ImGuiCol_ScrollbarGrab] = ImVec4(0.43f, 0.29f, 0.20f, 1.0f);
            break;

        case UiTheme::Frost:
            colours[ImGuiCol_Text] = ImVec4(0.11f, 0.15f, 0.21f, 1.0f);
            colours[ImGuiCol_TextDisabled] = ImVec4(0.38f, 0.44f, 0.52f, 1.0f);
            colours[ImGuiCol_WindowBg] = ImVec4(0.88f, 0.91f, 0.95f, 1.0f);
            colours[ImGuiCol_ChildBg] = ImVec4(0.82f, 0.86f, 0.92f, 1.0f);
            colours[ImGuiCol_PopupBg] = ImVec4(0.94f, 0.96f, 0.99f, 1.0f);
            colours[ImGuiCol_Border] = ImVec4(0.56f, 0.63f, 0.72f, 1.0f);
            colours[ImGuiCol_FrameBg] = ImVec4(0.72f, 0.78f, 0.87f, 1.0f);
            colours[ImGuiCol_FrameBgHovered] = ImVec4(0.56f, 0.68f, 0.84f, 1.0f);
            colours[ImGuiCol_FrameBgActive] = ImVec4(0.40f, 0.57f, 0.78f, 1.0f);
            colours[ImGuiCol_TitleBg] = ImVec4(0.77f, 0.83f, 0.91f, 1.0f);
            colours[ImGuiCol_TitleBgActive] = ImVec4(0.77f, 0.83f, 0.91f, 1.0f);
            colours[ImGuiCol_MenuBarBg] = ImVec4(0.72f, 0.79f, 0.88f, 1.0f);
            colours[ImGuiCol_Button] = ImVec4(0.28f, 0.48f, 0.72f, 1.0f);
            colours[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.42f, 0.70f, 1.0f);
            colours[ImGuiCol_ButtonActive] = ImVec4(0.14f, 0.34f, 0.61f, 1.0f);
            colours[ImGuiCol_Header] = ImVec4(0.43f, 0.59f, 0.79f, 0.70f);
            colours[ImGuiCol_HeaderHovered] = ImVec4(0.35f, 0.54f, 0.79f, 0.90f);
            colours[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.46f, 0.74f, 1.0f);
            colours[ImGuiCol_Separator] = ImVec4(0.56f, 0.63f, 0.72f, 1.0f);
            colours[ImGuiCol_ResizeGrip] = ImVec4(0.22f, 0.45f, 0.74f, 0.45f);
            colours[ImGuiCol_ScrollbarBg] = ImVec4(0.77f, 0.82f, 0.89f, 1.0f);
            colours[ImGuiCol_ScrollbarGrab] = ImVec4(0.47f, 0.56f, 0.67f, 1.0f);
            break;

        case UiTheme::Windows:
            colours[ImGuiCol_Text] = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
            colours[ImGuiCol_TextDisabled] = ImVec4(0.46f, 0.46f, 0.46f, 1.0f);
            colours[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.94f, 0.94f, 1.0f);
            colours[ImGuiCol_ChildBg] = ImVec4(0.96f, 0.96f, 0.96f, 1.0f);
            colours[ImGuiCol_PopupBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
            colours[ImGuiCol_Border] = ImVec4(0.78f, 0.78f, 0.78f, 1.0f);
            colours[ImGuiCol_FrameBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
            colours[ImGuiCol_FrameBgHovered] = ImVec4(0.96f, 0.96f, 0.96f, 1.0f);
            colours[ImGuiCol_FrameBgActive] = ImVec4(0.91f, 0.96f, 1.00f, 1.0f);
            colours[ImGuiCol_TitleBg] = ImVec4(0.91f, 0.91f, 0.91f, 1.0f);
            colours[ImGuiCol_TitleBgActive] = ImVec4(0.91f, 0.91f, 0.91f, 1.0f);
            colours[ImGuiCol_MenuBarBg] = ImVec4(0.93f, 0.93f, 0.93f, 1.0f);
            colours[ImGuiCol_Button] = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
            colours[ImGuiCol_ButtonHovered] = ImVec4(0.90f, 0.95f, 0.99f, 1.0f);
            colours[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.91f, 1.00f, 1.0f);
            colours[ImGuiCol_Header] = ImVec4(0.90f, 0.95f, 0.99f, 1.0f);
            colours[ImGuiCol_HeaderHovered] = ImVec4(0.80f, 0.91f, 1.00f, 1.0f);
            colours[ImGuiCol_HeaderActive] = ImVec4(0.60f, 0.82f, 1.00f, 1.0f);
            colours[ImGuiCol_Separator] = ImVec4(0.78f, 0.78f, 0.78f, 1.0f);
            colours[ImGuiCol_ResizeGrip] = ImVec4(0.00f, 0.47f, 0.83f, 0.35f);
            colours[ImGuiCol_ScrollbarBg] = ImVec4(0.94f, 0.94f, 0.94f, 1.0f);
            colours[ImGuiCol_ScrollbarGrab] = ImVec4(0.76f, 0.76f, 0.76f, 1.0f);
            break;

        case UiTheme::AdobePremiere:
            colours[ImGuiCol_Text] = ImVec4(0.816f, 0.816f, 0.816f, 1.0f);
            colours[ImGuiCol_TextDisabled] = ImVec4(0.522f, 0.522f, 0.522f, 1.0f);
            colours[ImGuiCol_WindowBg] = ImVec4(0.125f, 0.125f, 0.125f, 1.0f);
            colours[ImGuiCol_ChildBg] = ImVec4(0.141f, 0.141f, 0.141f, 1.0f);
            colours[ImGuiCol_PopupBg] = ImVec4(0.165f, 0.165f, 0.165f, 1.0f);
            colours[ImGuiCol_Border] = ImVec4(0.043f, 0.043f, 0.043f, 1.0f);
            colours[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 1.0f);
            colours[ImGuiCol_FrameBg] = ImVec4(0.090f, 0.090f, 0.090f, 1.0f);
            colours[ImGuiCol_FrameBgHovered] = ImVec4(0.157f, 0.157f, 0.157f, 1.0f);
            colours[ImGuiCol_FrameBgActive] = ImVec4(0.204f, 0.204f, 0.204f, 1.0f);
            colours[ImGuiCol_TitleBg] = ImVec4(0.106f, 0.106f, 0.106f, 1.0f);
            colours[ImGuiCol_TitleBgActive] = ImVec4(0.106f, 0.106f, 0.106f, 1.0f);
            colours[ImGuiCol_TitleBgCollapsed] = ImVec4(0.090f, 0.090f, 0.090f, 1.0f);
            colours[ImGuiCol_MenuBarBg] = ImVec4(0.094f, 0.094f, 0.094f, 1.0f);
            colours[ImGuiCol_ScrollbarBg] = ImVec4(0.082f, 0.082f, 0.082f, 1.0f);
            colours[ImGuiCol_ScrollbarGrab] = ImVec4(0.227f, 0.227f, 0.227f, 1.0f);
            colours[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.314f, 0.314f, 0.314f, 1.0f);
            colours[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.400f, 0.400f, 0.400f, 1.0f);
            colours[ImGuiCol_CheckMark] = ImVec4(0.078f, 0.451f, 0.902f, 1.0f);
            colours[ImGuiCol_SliderGrab] = ImVec4(0.078f, 0.451f, 0.902f, 1.0f);
            colours[ImGuiCol_SliderGrabActive] = ImVec4(0.149f, 0.518f, 1.00f, 1.0f);
            colours[ImGuiCol_Button] = ImVec4(0.188f, 0.188f, 0.188f, 1.0f);
            colours[ImGuiCol_ButtonHovered] = ImVec4(0.255f, 0.255f, 0.255f, 1.0f);
            colours[ImGuiCol_ButtonActive] = ImVec4(0.314f, 0.314f, 0.314f, 1.0f);
            colours[ImGuiCol_Header] = ImVec4(0.169f, 0.169f, 0.169f, 1.0f);
            colours[ImGuiCol_HeaderHovered] = ImVec4(0.227f, 0.227f, 0.227f, 1.0f);
            colours[ImGuiCol_HeaderActive] = ImVec4(0.078f, 0.451f, 0.902f, 1.0f);
            colours[ImGuiCol_Separator] = ImVec4(0.204f, 0.204f, 0.204f, 1.0f);
            colours[ImGuiCol_SeparatorHovered] = ImVec4(0.290f, 0.290f, 0.290f, 1.0f);
            colours[ImGuiCol_SeparatorActive] = ImVec4(0.078f, 0.451f, 0.902f, 1.0f);
            colours[ImGuiCol_ResizeGrip] = ImVec4(0.078f, 0.451f, 0.902f, 0.20f);
            colours[ImGuiCol_ResizeGripHovered] = ImVec4(0.078f, 0.451f, 0.902f, 0.67f);
            colours[ImGuiCol_ResizeGripActive] = ImVec4(0.078f, 0.451f, 0.902f, 1.0f);
            colours[ImGuiCol_Tab] = ImVec4(0.106f, 0.106f, 0.106f, 1.0f);
            colours[ImGuiCol_TabHovered] = ImVec4(0.161f, 0.161f, 0.161f, 1.0f);
            colours[ImGuiCol_TabSelected] = ImVec4(0.141f, 0.141f, 0.141f, 1.0f);
            colours[ImGuiCol_TabSelectedOverline] = ImVec4(0.078f, 0.451f, 0.902f, 1.0f);
            colours[ImGuiCol_TabDimmed] = ImVec4(0.086f, 0.086f, 0.086f, 1.0f);
            colours[ImGuiCol_TabDimmedSelected] = ImVec4(0.125f, 0.125f, 0.125f, 1.0f);
            colours[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.078f, 0.451f, 0.902f, 0.55f);
            colours[ImGuiCol_TextSelectedBg] = ImVec4(0.078f, 0.451f, 0.902f, 0.45f);
            colours[ImGuiCol_DragDropTarget] = ImVec4(0.078f, 0.451f, 0.902f, 1.0f);
            colours[ImGuiCol_NavHighlight] = ImVec4(0.078f, 0.451f, 0.902f, 1.0f);
            colours[ImGuiCol_TableHeaderBg] = ImVec4(0.161f, 0.161f, 0.161f, 1.0f);
            colours[ImGuiCol_TableBorderStrong] = ImVec4(0.043f, 0.043f, 0.043f, 1.0f);
            colours[ImGuiCol_TableBorderLight] = ImVec4(0.188f, 0.188f, 0.188f, 1.0f);
            colours[ImGuiCol_TableRowBg] = ImVec4(0.125f, 0.125f, 0.125f, 1.0f);
            colours[ImGuiCol_TableRowBgAlt] = ImVec4(0.141f, 0.141f, 0.141f, 1.0f);
            break;
        }
    }

    inline sf::Color CurrentThemeBackgroundColour()
    {
        const ImVec4 colour = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        const auto toChannel = [](float value)
        {
            return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return sf::Color(toChannel(colour.x), toChannel(colour.y), toChannel(colour.z), toChannel(colour.w));
    }

    inline bool RenderThemesMenu(UiTheme& theme)
    {
        bool changed = false;
        if (ImGui::BeginMenu("Themes"))
        {
            const auto selectTheme = [&theme, &changed](UiTheme candidate, const char* label)
            {
                if (ImGui::MenuItem(label, nullptr, theme == candidate))
                {
                    theme = candidate;
                    changed = true;
                }
            };

            selectTheme(UiTheme::MidnightBlue, "Midnight Blue");
            selectTheme(UiTheme::Ember, "Ember");
            selectTheme(UiTheme::Frost, "Frost");
            selectTheme(UiTheme::Windows, "Windows");
            selectTheme(UiTheme::AdobePremiere, "Adobe Premiere");
            ImGui::EndMenu();
        }
        return changed;
    }
}
