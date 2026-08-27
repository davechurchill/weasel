#pragma once

#include "ProjectData.h"

#include <vector>

namespace weasel
{
    // Full project-data snapshots for simple project-wide undo/redo.
    // record() ignores navigation-only differences through
    // ProjectData::sameContent().
    class EditHistory
    {
    private:
        std::vector<ProjectData> m_states;
        int                      m_currentIndex = NoIndex;
        int                      m_savedIndex = NoIndex;

    public:
        static constexpr int NoIndex = -1;

        // Starts a new, clean history with project data as both the current and
        // saved state. New and successfully opened projects use this path.
        void reset(const ProjectData& data);

        // Records changed project data. Redo entries are discarded first; when
        // that branch contained the saved entry, savedIndex becomes NoIndex.
        // Returns false when project data matches the current entry.
        bool record(const ProjectData& data);

        bool canUndo() const noexcept;
        bool canRedo() const noexcept;

        // Restores persistent content while preserving the caller's current
        // navigation-only playhead. The caller-owned data address stays
        // stable for timeline and UI references.
        bool undo(ProjectData& data);
        bool redo(ProjectData& data);
        bool restore(ProjectData& data) const;

        // Saving is a checkpoint, not an edit. Replace the current snapshot
        // with the successfully persisted data, then record its index.
        void markSaved(const ProjectData& data);
        bool isDirty() const noexcept;

    };
}
