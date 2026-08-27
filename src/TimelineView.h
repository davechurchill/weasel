#pragma once

#include <imgui.h>

namespace weasel
{
    class Editor;

    // Owns the ImGui presentation and pointer interaction for the sequence
    // timeline. It renders directly against the editor's project, timeline
    // state, and media controllers.
    class TimelineView
    {
    public:
        void render(Editor& editor, const ImVec2& position, const ImVec2& size);
    };
}
