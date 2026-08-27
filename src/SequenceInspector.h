#pragma once

namespace weasel
{
    class Editor;

    // Renders the controls that configure the active sequence.  This is a
    // built-in part of the editor UI, so it talks to its owning Editor
    // directly instead of going through a generic view context.
    class SequenceInspector
    {
    public:
        void render(Editor& editor) const;
    };
}
