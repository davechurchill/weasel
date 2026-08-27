#pragma once

#include "project/ClipPresetLibrary.h"
#include "ui/ClipInspector.h"
#include "app/EditorState.h"
#include "render/ExportController.h"
#include "media/MediaImportController.h"
#include "media/MediaThumbnailController.h"
#include "render/PreviewController.h"
#include "media/PreviewFrameCache.h"
#include "app/RecentProjects.h"
#include "render/SequenceAudioController.h"
#include "ui/SequenceInspector.h"
#include "app/Themes.hpp"
#include "timeline/TimelineController.h"
#include "ui/TimelineView.h"
#include "timeline/WaveformAlignment.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <Windows.h>
#endif

#include <SFML/Graphics.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace weasel
{
    enum class TimelineTool
    {
        Selection,
        Cut
    };

    struct TimelinePresentationState
    {
        static constexpr float DefaultPixelsPerSecond = 80.0f;
        static constexpr float DefaultLayerHeight = 58.0f;

        float        pixelsPerSecond = DefaultPixelsPerSecond;
        bool         fitToSequence = false;
        bool         snapToEdges = true;
        float        videoLayerHeight = DefaultLayerHeight;
        float        audioLayerHeight = DefaultLayerHeight;
        TimelineTool tool = TimelineTool::Selection;
        bool         draggingPlayhead = false;
    };

    struct EditorUiState
    {
        TimelinePresentationState timeline;
        WaveformAlignmentRequest  waveformAlignment;
        int                       batchImageDurationMs = 4000;
        bool                      monitorPlayheadSliderActive = false;
        UiTheme                   theme = UiTheme::MidnightBlue;

        void resetForProjectChange() noexcept
        {
            waveformAlignment.clear();
            monitorPlayheadSliderActive = false;
            timeline.draggingPlayhead = false;
        }
    };

    class Editor
    {
    private:
        enum class LayoutSplitter
        {
            None,
            MediaMonitor,
            MonitorInspector,
            UpperTimeline
        };

        enum class PendingProjectAction
        {
            None,
            NewProject,
            OpenProjectDialog,
            OpenProjectFolder,
            Quit
        };

        enum class ProjectPanelTab
        {
            Project,
            Sequence,
            Media,
            Export
        };

        struct NativeFileDrop
        {
            std::vector<std::filesystem::path> paths;
            sf::Vector2i                       clientPosition{};
            bool                               releasedInClientArea = false;
        };

        struct PendingTimelineFileDrop
        {
            std::vector<int> assetIds;
            ImVec2           position{};
        };

        struct PendingMediaImport
        {
            std::vector<std::filesystem::path> paths;
            std::vector<int>                   importedAssetIds;
            std::size_t                        nextPath = 0;
            sf::Vector2i                       dropPosition{};
            bool                               addToTimeline = false;
            bool                               progressShown = false;
        };

        struct PendingProjectLoad
        {
            std::vector<int> legacyVideoAssetIds;
            std::size_t      nextAsset = 0;
            bool             projectChanged = false;
            bool             progressShown = false;
        };

        sf::RenderWindow        m_window;
        sf::Clock               m_clock;
        bool                    m_imguiInitialized = false;
        bool                    m_running = true;
        std::string             m_windowTitle;
        std::filesystem::path   m_applicationDirectory;
        std::filesystem::path   m_dataDirectory;
        RecentProjects           m_recentProjects;

        // EditorState owns the project data, file path, dirty flag, and timeline
        // history. The editor keeps stable references for the duration of the
        // application and lets its built-in panels work with them directly.
        EditorState               m_editorState;
        ProjectData&              m_project;
        PendingProjectAction      m_pendingProjectAction = PendingProjectAction::None;
        std::filesystem::path     m_pendingOpenProjectDirectory;
        std::string               m_unsavedChangesError;
        TimelineController&       m_timelineController;
        MediaImportController     m_mediaImportController;
        ExportController          m_exportController;
        PreviewFrameCache         m_previewFrameCache;
        PreviewController         m_previewController;
        MediaThumbnailController  m_mediaThumbnailController;
        SequenceAudioController   m_sequenceAudioController;
        ClipPresetLibrary         m_clipPresetLibrary;
        EditorUiState             m_uiState;
        ClipInspector             m_clipInspector;
        SequenceInspector         m_sequenceInspector;
        TimelineView              m_timelineView;
        std::array<char, 128>     m_projectNameInput{};
        std::array<char, 128>     m_exportNameInput{};
        bool                      m_openUnsavedChangesModalRequested = false;
        bool                      m_openEncodingWindowRequested = false;
        bool                      m_closeUnsavedChangesModalRequested = false;
        bool                      m_openProjectTab = true;
        bool                      m_openMediaTab = false;
        bool                      m_openExportTab = false;
        bool                      m_openClipTab = false;
        bool                      m_openSequenceTab = false;
        ProjectPanelTab           m_activeProjectPanelTab = ProjectPanelTab::Project;
        int&                      m_selectedAssetId;
        bool                      m_playing = false;
        // Waveform extraction can be expensive, so leave it opt-in until the
        // user enables it from the Sequence tab.

        // These are deliberately application-layout settings, not project settings.
        // Panels remain fixed in place while the seams between them are dragged.
        float                   m_mediaPanelWidth = 0.0f;
        float                   m_inspectorPanelWidth = 0.0f;
        float                   m_timelinePanelHeight = 0.0f;
        LayoutSplitter          m_activeLayoutSplitter = LayoutSplitter::None;
        std::mutex              m_droppedFilesMutex;
        inline static Editor*   s_fileDropTarget = nullptr;


        std::vector<NativeFileDrop>                 m_droppedFiles;
        std::vector<PendingTimelineFileDrop>        m_pendingTimelineFileDrops;
        std::deque<PendingMediaImport>               m_pendingMediaImports;
        std::optional<PendingProjectLoad>            m_pendingProjectLoad;

        void*                   m_nativeWindow = nullptr;
#ifdef _WIN32
        WNDPROC                 m_previousWindowProc = nullptr;
#endif
        void update();
        void processEvents();
        void processEvent(const sf::Event& event);
        void renderUI();
        void renderUnsavedChangesModal();
        void renderOperationProgressModal();
        void renderMenu();
        void renderProjectPanel(const ImVec2& position, const ImVec2& size);
        void renderVideoPreview(const ImVec2& position, const ImVec2& size);
        void renderInspector(const ImVec2& position, const ImVec2& size);
        void renderTimeline(const ImVec2& position, const ImVec2& size);
        void processEncodingEvents();
        void renderEncodingWindow(sf::Time deltaTime);
        bool openEncodingWindow();
        void closeEncodingWindow(bool cancelExport);

        void importMediaDialog();
        bool importMedia(const std::filesystem::path& path);
        void queueMediaImport(std::vector<std::filesystem::path> paths,
                              std::optional<sf::Vector2i> dropPosition = std::nullopt);
        void processPendingMediaImports();
        std::optional<std::filesystem::path> chooseCubeLutFile(
            const std::filesystem::path& currentLutPath);
        TimelineClip* addMediaToTimeline(int assetId, int trackIndex, double timelineStart);
        bool processDroppedFiles();
        void openProjectDialog();
        bool openProject(const std::filesystem::path& projectDirectory);
        void processPendingProjectLoad();
        void finishOpenProject();
        void requestProjectAction(PendingProjectAction action,
                                  std::filesystem::path projectDirectory = {});
        void executePendingProjectAction();
        void clearPendingProjectAction();
        void startNewProject();
        void quitApplication();
        bool saveProject();
        bool saveProjectAsNamed();
        void updateSequenceAudioCacheDirectory();
        void rememberCurrentProject();
        void loadClipPresets();
        std::optional<int> maximumSequenceSourceVideoBitrateKbps(bool& refreshedAssetMetadata);
        void startExport(std::filesystem::path output = {});
        void exportAsDialog();
        void requestSequencePreview();
        void updateSequenceAudio();
        void requestScrubAudio();
        void invalidatePreview();
        void resetPreview();
        void updateWindowTitle();
        bool hasUnsavedChanges();
        void beginSequenceUndoTransaction(bool timelineDrag = false);
        bool commitSequenceUndoTransaction();
        void discardSequenceUndoTransaction();
        void commitPendingDocumentEdits();
        void afterTimelineHistoryRestore();
        bool canUndoProject() const;
        bool canRedoProject() const;
        void undoProject();
        void redoProject();
        bool canCopySelectedClip() const;
        bool canPasteClip() const;
        bool hasSelectedMedia() const;
        void copySelectedClip();
        bool pasteCopiedClip();
        bool deleteSelectedClip();
        bool deleteSelectedMedia();
        void alignSelectedClipsByWaveform(int anchorClipId, int movingClipId);
        void clearWaveformAlignment() noexcept;

        void endTimelineDrag();

        void setupNativeFileDrop();
        void teardownNativeFileDrop();
#ifdef _WIN32
        static LRESULT CALLBACK fileDropWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
#endif

        friend class ClipInspector;
        friend class SequenceInspector;
        friend class TimelineView;

    public:
        Editor();
        ~Editor();

        Editor(const Editor&) = delete;
        Editor& operator=(const Editor&) = delete;

        void run();
    };
}
