#include "EditorState.h"

#include "ProjectFile.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <utility>

namespace
{
    std::atomic<std::uint64_t> NextUnsavedCacheDirectory{ 1 };

    std::filesystem::path MakeUnsavedCacheDirectory()
    {
        std::error_code error;
        const std::filesystem::path temporaryDirectory = std::filesystem::temp_directory_path(error);
        if (error)
        {
            return {};
        }

        const std::uint64_t nonce = NextUnsavedCacheDirectory.fetch_add(1, std::memory_order_relaxed);
        const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return temporaryDirectory / ("weasel-unsaved-" + std::to_string(timestamp)
            + "-" + std::to_string(nonce) + "-cache");
    }

    bool SameDirectory(const std::filesystem::path& first,
                       const std::filesystem::path& second)
    {
        if (first.empty() || second.empty())
        {
            return false;
        }

        std::error_code error;
        if (std::filesystem::equivalent(first, second, error))
        {
            return true;
        }

        error.clear();
        const std::filesystem::path absoluteFirst = std::filesystem::absolute(first, error);
        if (error)
        {
            return false;
        }

        const std::filesystem::path absoluteSecond = std::filesystem::absolute(second, error);
        return !error && absoluteFirst.lexically_normal() == absoluteSecond.lexically_normal();
    }
}

namespace weasel
{
    EditorState::EditorState()
        : m_timeline(m_project, [this] { recordChange(); })
    {
        refreshUnsavedCacheDirectory();
        resetHistory();
    }

    ProjectData& EditorState::project() noexcept
    {
        return m_project;
    }

    const ProjectData& EditorState::project() const noexcept
    {
        return m_project;
    }

    TimelineController& EditorState::timeline() noexcept
    {
        return m_timeline;
    }

    const TimelineController& EditorState::timeline() const noexcept
    {
        return m_timeline;
    }

    const std::filesystem::path& EditorState::projectDirectory() const noexcept
    {
        return m_projectDirectory;
    }

    bool EditorState::hasProjectDirectory() const noexcept
    {
        return !m_projectDirectory.empty();
    }

    std::filesystem::path EditorState::cacheDirectory() const
    {
        return m_projectDirectory.empty()
            ? m_unsavedCacheDirectory
            : m_projectDirectory / "cache";
    }

    std::filesystem::path EditorState::exportDirectory() const
    {
        return m_projectDirectory.empty() ? std::filesystem::path{} : m_projectDirectory / "exports";
    }

    bool EditorState::isDirty() const noexcept
    {
        return m_history.isDirty();
    }

    void EditorState::recordChange()
    {
        (void)m_history.record(m_project);
    }

    bool EditorState::canUndo() const noexcept
    {
        return m_history.canUndo();
    }

    bool EditorState::canRedo() const noexcept
    {
        return m_history.canRedo();
    }

    bool EditorState::undo()
    {
        if (!m_history.canUndo())
        {
            return false;
        }

        m_timeline.prepareForHistoryRestore();
        if (!m_history.undo(m_project))
        {
            return false;
        }
        m_timeline.revalidateSelection();
        return true;
    }

    bool EditorState::redo()
    {
        if (!m_history.canRedo())
        {
            return false;
        }

        m_timeline.prepareForHistoryRestore();
        if (!m_history.redo(m_project))
        {
            return false;
        }
        m_timeline.revalidateSelection();
        return true;
    }

    void EditorState::resetHistory()
    {
        m_history.reset(m_project);
    }

    void EditorState::markSaved()
    {
        // Saving can normalize persisted fields or commit a Save As name, but
        // it is a checkpoint rather than a new undo action.
        m_history.markSaved(m_project);
    }

    void EditorState::resetNewProject()
    {
        m_project.reset();
        m_projectDirectory.clear();
        refreshUnsavedCacheDirectory();
        m_timeline.resetForProjectChange();
        resetHistory();
    }

    bool EditorState::save(std::string& error)
    {
        if (m_projectDirectory.empty())
        {
            error = "Choose a project folder first.";
            return false;
        }
        return saveAs(m_projectDirectory, error);
    }

    bool EditorState::saveAs(const std::filesystem::path& projectDirectory,
                             std::string& error,
                             std::optional<std::string> projectName)
    {
        if (projectDirectory.empty())
        {
            error = "Choose a project folder first.";
            return false;
        }

        const std::filesystem::path projectFile = ProjectFileDetail::ProjectJsonPath(projectDirectory);
        std::error_code filesystemError;
        if (std::filesystem::exists(projectFile, filesystemError))
        {
            if (!SameDirectory(m_projectDirectory, projectDirectory))
            {
                error = "That folder already contains a Weasel project. Open it or choose another folder.";
                return false;
            }
        }
        else if (filesystemError)
        {
            error = "Could not inspect the project folder: " + filesystemError.message();
            return false;
        }
        else if (!SameDirectory(m_projectDirectory, projectDirectory)
                 && std::filesystem::exists(projectDirectory, filesystemError))
        {
            if (filesystemError)
            {
                error = "Could not inspect the project folder: " + filesystemError.message();
                return false;
            }
            if (!std::filesystem::is_empty(projectDirectory, filesystemError))
            {
                if (filesystemError)
                {
                    error = "Could not inspect the project folder: " + filesystemError.message();
                }
                else
                {
                    error = "That folder already contains files. Choose a new project name or an empty folder.";
                }
                return false;
            }
        }

        // Save As changes both the state folder and, when requested by the
        // caller, the logical project name written into JSON.  Keep the
        // in-memory project untouched if persistence fails so a clean
        // project never gains an unsaved name-only mutation.
        std::optional<std::string> previousName;
        if (projectName && m_project.name() != *projectName)
        {
            previousName = m_project.name();
            m_project.name() = std::move(*projectName);
        }
        if (!SaveProjectFile(m_project, projectDirectory, error))
        {
            if (previousName)
            {
                m_project.name() = std::move(*previousName);
            }
            return false;
        }

        m_projectDirectory = projectDirectory;
        markSaved();
        error.clear();
        return true;
    }

    bool EditorState::load(const std::filesystem::path& projectDirectory,
                           std::string& error)
    {
        ProjectData loaded;
        if (!LoadProjectFile(loaded, projectDirectory, error))
        {
            return false;
        }

        // Assignment keeps m_project's address stable, so TimelineController
        // continues to refer to the active project.
        m_project = std::move(loaded);
        m_projectDirectory = projectDirectory;
        m_timeline.resetForProjectChange();
        resetHistory();
        error.clear();
        return true;
    }

    void EditorState::refreshUnsavedCacheDirectory()
    {
        m_unsavedCacheDirectory = MakeUnsavedCacheDirectory();
    }
}
