#pragma once

#include "project/ProjectData.h"
#include "render/VideoExporter.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace sf
{
    class RenderWindow;
    class Time;
}

namespace weasel
{
    // Owns the asynchronous exporter and its separate encoding-progress
    // window. The caller retains ownership of the primary render window and
    // should call closeEncodingWindow() before shutting its ImGui-SFML context
    // down.
    class ExportController
    {
    public:
        struct ImGuiCallbacks
        {
            // Lets the application use the same look in the encoding window
            // without coupling this controller to a particular editor class.
            // It is invoked when the progress window opens and on an explicit
            // refresh request, so a live theme change remains in sync.
            std::function<void()> applyStyle;
            // Optional hook for integrations that maintain a dynamic font
            // atlas and need to upload pending changes in every ImGui context.
            std::function<void()> uploadFontAtlas;
        };

    private:
        VideoExporter                         m_exporter;
        ImGuiCallbacks                        m_imguiCallbacks;
        std::unique_ptr<sf::RenderWindow>     m_encodingWindow;
        bool                                  m_encodingImGuiInitialized = false;
        bool                                  m_encodingStyleRefreshRequested = false;

        [[nodiscard]] bool openEncodingWindow(sf::RenderWindow& mainWindow, std::string& error);
        void destroyEncodingWindow();
        static void restoreMainWindowContext(sf::RenderWindow& mainWindow);

    public:
        explicit ExportController(ImGuiCallbacks callbacks = {});
        ~ExportController();

        ExportController(const ExportController&) = delete;
        ExportController& operator=(const ExportController&) = delete;

        // Starts an export and opens its progress window. The project is
        // copied by the exporter, including its export settings, so the job
        // cannot observe concurrent editor mutations.
        [[nodiscard]] bool startExport(sf::RenderWindow& mainWindow,
                                       const ProjectData& project,
                                       const std::filesystem::path& ffmpegPath,
                                       const std::filesystem::path& outputPath,
                                       std::string& error);

        // Brings an already-open progress window forward, or reopens it after
        // it was closed while a job is still active.
        [[nodiscard]] bool showEncodingWindow(sf::RenderWindow& mainWindow, std::string& error);

        // Call these from the primary application's event/update phases.
        // Both methods restore the main ImGui-SFML/OpenGL context before they
        // return, allowing normal primary-window UI rendering to continue.
        void processEncodingEvents(sf::RenderWindow& mainWindow);
        void renderEncodingWindow(sf::RenderWindow& mainWindow, const sf::Time& deltaTime);

        // Requests that the next encoding-window frame refresh its ImGui
        // style. This is safe to call while the primary ImGui context is live.
        void requestEncodingStyleRefresh() noexcept;

        // Closing from the encoding window's title bar cancels the active job;
        // callers can pass false when the application itself is shutting down.
        void closeEncodingWindow(sf::RenderWindow& mainWindow, bool cancelExport);

        void cancel();
        ExportStatus status() const;
    };
}
