#pragma once

#include <imgui.h>

#include <cstring>

namespace weasel
{
    inline ImGuiWindowFlags FixedPanelWindowFlags() noexcept
    {
        return ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoBringToFrontOnFocus;
    }

    inline ImTextureID ImGuiTextureId(unsigned int textureHandle) noexcept
    {
        static_assert(sizeof(textureHandle) <= sizeof(ImTextureID));
        ImTextureID textureId{};
        std::memcpy(&textureId, &textureHandle, sizeof(textureHandle));
        return textureId;
    }
}
