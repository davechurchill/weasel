#pragma once

#include <array>

namespace weasel
{
    class Editor;

    // The selected-clip inspector. It renders directly against the editor's
    // active project data and session state; it does not own project state.
    class ClipInspector
    {
    private:
        std::array<char, 128> m_presetNameInput{};
        int                   m_selectedPresetIndex = -1;

    public:
        enum class View
        {
            Video,
            Effects,
            Audio
        };

        void render(Editor& editor, View view);

        void resetPresetSelection() noexcept;
    };
}
