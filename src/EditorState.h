#pragma once

#include "EditHistory.h"
#include "ProjectData.h"
#include "TimelineController.h"

#include <filesystem>
#include <optional>
#include <string>

namespace weasel
{
    // The small, concrete owner for everything around the open project:
    // project folder, project-wide history, sidecar directories, and timeline UI.
    // ProjectData itself remains persistent content only.
    class EditorState
    {
    private:
        ProjectData             m_project;
        EditHistory             m_history;
        std::filesystem::path   m_projectDirectory;
        std::filesystem::path   m_unsavedCacheDirectory;
        TimelineController      m_timeline;

        void refreshUnsavedCacheDirectory();
        void resetHistory();
        void markSaved();

    public:
        EditorState();

        EditorState(const EditorState&) = delete;
        EditorState& operator=(const EditorState&) = delete;
        EditorState(EditorState&&) = delete;
        EditorState& operator=(EditorState&&) = delete;
        ~EditorState() = default;

        ProjectData& project() noexcept;
        const ProjectData& project() const noexcept;
        TimelineController& timeline() noexcept;
        const TimelineController& timeline() const noexcept;

        const std::filesystem::path& projectDirectory() const noexcept;
        bool hasProjectDirectory() const noexcept;
        // Saved projects use <project-directory>/cache.
        // Unsaved projects use a fresh temporary directory so audio preview
        // and waveforms remain available without creating shared app data.
        std::filesystem::path cacheDirectory() const;
        // Saved projects use <project-directory>/exports.
        // Unsaved projects have no default export directory.
        std::filesystem::path exportDirectory() const;

        bool isDirty() const noexcept;

        // Records the current persistent project state. A new record drops
        // any redo branch; EditHistory handles the saved-checkpoint rules.
        void recordChange();
        bool canUndo() const noexcept;
        bool canRedo() const noexcept;
        bool undo();
        bool redo();

        void resetNewProject();

        // Persistence stays at the state boundary.
        [[nodiscard]] bool save(std::string& error);
        [[nodiscard]] bool saveAs(const std::filesystem::path& projectDirectory,
                                  std::string& error,
                                  std::optional<std::string> projectName = std::nullopt);
        [[nodiscard]] bool load(const std::filesystem::path& projectDirectory,
                                std::string& error);

    };
}
