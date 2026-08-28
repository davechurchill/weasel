#include "render/ExportController.h"

#include <SFML/Graphics.hpp>
#include <imgui.h>
#include <imgui-SFML.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <utility>

namespace
{
    std::string EstimatedTimeText(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0.0)
        {
            return "Calculating...";
        }

        const int totalSeconds = static_cast<int>(std::ceil(seconds));
        const int hours = totalSeconds / 3600;
        const int minutes = (totalSeconds % 3600) / 60;
        const int remainingSeconds = totalSeconds % 60;
        char buffer[32]{};
        if (hours > 0)
        {
            std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, remainingSeconds);
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, remainingSeconds);
        }
        return buffer;
    }

    std::string ProjectedFileSizeText(std::uint64_t bytes)
    {
        if (bytes == 0)
        {
            return "Calculating...";
        }

        constexpr std::array<const char*, 5> units = { "B", "KB", "MB", "GB", "TB" };
        double value = static_cast<double>(bytes);
        std::size_t unitIndex = 0;
        while (value >= 1024.0 && unitIndex + 1 < units.size())
        {
            value /= 1024.0;
            ++unitIndex;
        }

        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), value < 10.0 && unitIndex > 0 ? "%.1f %s" : "%.0f %s",
                      value, units[unitIndex]);
        return buffer;
    }

    ImTextureID ImGuiTextureId(unsigned int textureHandle)
    {
        static_assert(sizeof(textureHandle) <= sizeof(ImTextureID));
        ImTextureID textureId{};
        std::memcpy(&textureId, &textureHandle, sizeof(textureHandle));
        return textureId;
    }

    void DrawTexture(const sf::Texture& texture, const ImVec2& size)
    {
        const ImTextureID textureId = ImGuiTextureId(texture.getNativeHandle());
#if IMGUI_VERSION_NUM >= 19200
        ImGui::Image(ImTextureRef(textureId), size);
#else
        ImGui::Image(textureId, size);
#endif
    }

}

namespace weasel
{
    ExportController::ExportController(ImGuiCallbacks callbacks)
        : m_imguiCallbacks(std::move(callbacks))
    {
    }

    ExportController::~ExportController()
    {
        // Normal application shutdown should call closeEncodingWindow() while
        // the primary ImGui-SFML context still exists. This fallback still
        // releases the secondary window when a caller omits that cleanup.
        m_exporter.cancel();
        destroyEncodingWindow();
    }

    bool ExportController::startExport(sf::RenderWindow& mainWindow,
                                       const ProjectData& project,
                                       const std::filesystem::path& ffmpegPath,
                                       const std::filesystem::path& outputPath,
                                       std::string& error)
    {
        m_startupError.clear();
        m_hasExportPreview = false;
        if (!showEncodingWindow(mainWindow, error))
        {
            return false;
        }

        if (m_exporter.start(project, ffmpegPath, outputPath, error))
        {
            return true;
        }

        m_startupError = error.empty() ? "Could not start the export." : error;
        return false;
    }

    bool ExportController::showEncodingWindow(sf::RenderWindow& mainWindow, std::string& error)
    {
        if (m_encodingWindow && m_encodingWindow->isOpen())
        {
            m_encodingWindow->requestFocus();
            restoreMainWindowContext(mainWindow);
            error.clear();
            return true;
        }

        return openEncodingWindow(mainWindow, error);
    }

    void ExportController::processEncodingEvents(sf::RenderWindow& mainWindow)
    {
        if (!m_encodingWindow)
        {
            restoreMainWindowContext(mainWindow);
            return;
        }
        if (!m_encodingWindow->isOpen())
        {
            closeEncodingWindow(mainWindow, true);
            return;
        }

        ImGui::SFML::SetCurrentWindow(*m_encodingWindow);
        static_cast<void>(m_encodingWindow->setActive(true));
        while (const std::optional event = m_encodingWindow->pollEvent())
        {
            ImGui::SFML::ProcessEvent(*m_encodingWindow, *event);
            if (event->is<sf::Event::Closed>())
            {
                closeEncodingWindow(mainWindow, true);
                return;
            }
        }

        restoreMainWindowContext(mainWindow);
    }

    void ExportController::renderEncodingWindow(sf::RenderWindow& mainWindow, const sf::Time& deltaTime)
    {
        if (!m_encodingWindow || !m_encodingWindow->isOpen() || !m_encodingImGuiInitialized)
        {
            restoreMainWindowContext(mainWindow);
            return;
        }

        ImGui::SFML::SetCurrentWindow(*m_encodingWindow);
        static_cast<void>(m_encodingWindow->setActive(true));
        if (m_encodingStyleRefreshRequested && m_imguiCallbacks.applyStyle)
        {
            m_imguiCallbacks.applyStyle();
        }
        m_encodingStyleRefreshRequested = false;
        ImGui::SFML::Update(*m_encodingWindow, deltaTime);

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
        const ImGuiWindowFlags contentFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        ImGui::Begin("Encoding Content", nullptr, contentFlags);

        const ExportStatus exportStatus = m_exporter.status();
        const double clampedProgress = std::clamp(exportStatus.progress, 0.0, 1.0);
        const bool finalizing = exportStatus.state == ExportState::Running && clampedProgress >= 1.0;
        const double displayedProgress = finalizing ? 0.99 : clampedProgress;
        char progressText[64]{};
        if (finalizing)
        {
            std::snprintf(progressText, sizeof(progressText), "Finalizing...");
        }
        else
        {
            std::snprintf(progressText, sizeof(progressText), "%.0f%%", clampedProgress * 100.0);
        }
        const bool exportComplete = exportStatus.state == ExportState::Succeeded;
        if (exportComplete)
        {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.22f, 0.72f, 0.40f, 1.0f));
        }
        ImGui::ProgressBar(static_cast<float>(displayedProgress), ImVec2(-1.0f, 0.0f), progressText);
        if (exportComplete)
        {
            ImGui::PopStyleColor();
        }
        ImGui::TextDisabled("Time remaining: %s", EstimatedTimeText(exportStatus.estimatedRemainingSeconds).c_str());
        ImGui::TextDisabled("Total elapsed: %s", EstimatedTimeText(exportStatus.elapsedSeconds).c_str());
        ImGui::TextDisabled("Projected file size: %s", ProjectedFileSizeText(exportStatus.projectedFileSizeBytes).c_str());

        if (!exportStatus.message.empty())
        {
            const ImVec4 messageColour = exportStatus.state == ExportState::Failed
                ? ImVec4(1.0f, 0.43f, 0.43f, 1.0f)
                : exportStatus.state == ExportState::Succeeded
                    ? ImVec4(0.39f, 0.85f, 0.55f, 1.0f)
                    : exportStatus.state == ExportState::Cancelled
                        ? ImVec4(0.95f, 0.72f, 0.32f, 1.0f)
                        : ImVec4(0.55f, 0.76f, 1.0f, 1.0f);
            ImGui::TextColored(messageColour, "%s", exportStatus.message.c_str());
        }
        if (!exportStatus.outputPath.empty())
        {
            ImGui::TextDisabled("%s", exportStatus.outputPath.string().c_str());
        }

        if (!m_startupError.empty())
        {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.43f, 0.43f, 1.0f), "Export could not start");
            ImGui::TextWrapped("%s", m_startupError.c_str());
        }

        bool showPreview = m_exporter.previewEnabled();
        if (ImGui::Checkbox("Show current frame (updates once per second)", &showPreview))
        {
            m_exporter.setPreviewEnabled(showPreview);
            if (!showPreview)
            {
                m_hasExportPreview = false;
            }
        }
        if (showPreview)
        {
            if (std::optional<ExportPreviewFrame> frame = m_exporter.takePreviewFrame())
            {
                const sf::Vector2u size(static_cast<unsigned int>(frame->width),
                                        static_cast<unsigned int>(frame->height));
                if (!m_exportPreviewTexture)
                {
                    m_exportPreviewTexture = std::make_unique<sf::Texture>();
                }
                if (m_exportPreviewTexture->getSize() == size || m_exportPreviewTexture->resize(size))
                {
                    m_exportPreviewTexture->update(frame->rgba.data());
                    m_hasExportPreview = true;
                }
            }
            if (m_hasExportPreview)
            {
                const sf::Vector2u size = m_exportPreviewTexture->getSize();
                const float width = static_cast<float>(size.x);
                const float height = static_cast<float>(size.y);
                const float scale = ImGui::GetContentRegionAvail().x / width;
                DrawTexture(*m_exportPreviewTexture, ImVec2(width * scale, height * scale));
            }
            else
            {
                ImGui::TextDisabled("Waiting for the next rendered frame...");
            }
        }

        if (exportStatus.state == ExportState::Running)
        {
            ImGui::BeginDisabled(exportStatus.cancelRequested);
            if (ImGui::Button("Cancel Export", ImVec2(-1.0f, 0.0f)))
            {
                m_exporter.cancel();
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(exportStatus.cancelRequested || exportStatus.finishRequested);
            if (ImGui::Button("Finish Export Now", ImVec2(-1.0f, 0.0f)))
            {
                m_exporter.finishNow();
            }
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("FFmpeg details", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const float detailsHeight = std::max(80.0f, ImGui::GetContentRegionAvail().y);
            if (ImGui::BeginChild("EncodingFfmpegDetails", ImVec2(0.0f, detailsHeight), ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_None))
            {
                ImGui::PushTextWrapPos(0.0f);
                if (!exportStatus.ffmpegCommand.empty())
                {
                    ImGui::TextUnformatted(exportStatus.ffmpegCommand.c_str());
                    if (!exportStatus.log.empty())
                    {
                        ImGui::Spacing();
                    }
                }
                if (exportStatus.log.empty())
                {
                    ImGui::TextDisabled("Waiting for FFmpeg output...");
                }
                else
                {
                    ImGui::TextUnformatted(exportStatus.log.c_str());
                }
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
        }
        ImGui::End();

        if (m_imguiCallbacks.uploadFontAtlas)
        {
            m_imguiCallbacks.uploadFontAtlas();
        }
        m_encodingWindow->clear(sf::Color(18, 20, 26));
        ImGui::SFML::Render(*m_encodingWindow);
        m_encodingWindow->display();

        restoreMainWindowContext(mainWindow);
    }

    void ExportController::closeEncodingWindow(sf::RenderWindow& mainWindow, bool cancelExport)
    {
        if (cancelExport && m_exporter.isRunning())
        {
            m_exporter.cancel();
        }

        destroyEncodingWindow();
        restoreMainWindowContext(mainWindow);
    }

    void ExportController::cancel()
    {
        m_exporter.cancel();
    }

    ExportStatus ExportController::status() const
    {
        return m_exporter.status();
    }

    void ExportController::requestEncodingStyleRefresh() noexcept
    {
        m_encodingStyleRefreshRequested = true;
    }

    bool ExportController::openEncodingWindow(sf::RenderWindow& mainWindow, std::string& error)
    {
        destroyEncodingWindow();

        try
        {
            m_encodingWindow = std::make_unique<sf::RenderWindow>(
                sf::VideoMode({ 720, 700 }), "Weasel - Encoding", sf::Style::Default);
            if (!m_encodingWindow->isOpen())
            {
                m_encodingWindow.reset();
                error = "Could not open the encoding window.";
                restoreMainWindowContext(mainWindow);
                return false;
            }

            if (!ImGui::SFML::Init(*m_encodingWindow))
            {
                // Init creates a context before it uploads the font texture,
                // so tear it down even when its final setup reports failure.
                ImGui::SFML::Shutdown(*m_encodingWindow);
                m_encodingWindow->close();
                m_encodingWindow.reset();
                error = "Could not initialize the encoding window UI.";
                restoreMainWindowContext(mainWindow);
                return false;
            }

            m_encodingImGuiInitialized = true;
            ImGui::GetIO().IniFilename = nullptr;
            if (m_imguiCallbacks.applyStyle)
            {
                m_imguiCallbacks.applyStyle();
            }
            m_encodingStyleRefreshRequested = false;
            m_encodingWindow->requestFocus();
            error.clear();
        }
        catch (const std::exception& exception)
        {
            destroyEncodingWindow();
            error = std::string("Could not open the encoding window: ") + exception.what();
            restoreMainWindowContext(mainWindow);
            return false;
        }

        restoreMainWindowContext(mainWindow);
        return true;
    }

    void ExportController::destroyEncodingWindow()
    {
        if (!m_encodingWindow)
        {
            return;
        }

        static_cast<void>(m_encodingWindow->setActive(true));
        m_exportPreviewTexture.reset();
        m_hasExportPreview = false;
        if (m_encodingImGuiInitialized)
        {
            ImGui::SFML::Shutdown(*m_encodingWindow);
            m_encodingImGuiInitialized = false;
        }
        m_encodingWindow->close();
        m_encodingWindow.reset();
    }

    void ExportController::restoreMainWindowContext(sf::RenderWindow& mainWindow)
    {
        if (!mainWindow.isOpen())
        {
            return;
        }

        ImGui::SFML::SetCurrentWindow(mainWindow);
        static_cast<void>(mainWindow.setActive(true));
    }
}
