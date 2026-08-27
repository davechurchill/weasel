#include "app/Editor.h"

#include "media/MediaProbe.h"
#include "media/MediaTools.h"
#include "timeline/WaveformAlignment.h"
#include <SFML/Graphics/Image.hpp>
#include <SFML/OpenGL.hpp>
#include <imgui.h>
#include <imgui-SFML.h>

#ifdef _WIN32
#include <commdlg.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    constexpr float PanelGap = 6.0f;
    constexpr float MinimumMediaPanelWidth = 220.0f;
    constexpr float MinimumMonitorWidth = 260.0f;
    constexpr float MinimumInspectorPanelWidth = 240.0f;
    constexpr float MinimumTimelinePanelHeight = 180.0f;
    constexpr float MinimumUpperPanelHeight = 180.0f;
    std::filesystem::path ApplicationDirectory()
    {
#ifdef _WIN32
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length > 0 && length < path.size())
        {
            return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
        }
#elif defined(__linux__)
        std::array<char, 4096> path{};
        const ssize_t length = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
        if (length > 0 && static_cast<std::size_t>(length) < path.size())
        {
            path[static_cast<std::size_t>(length)] = '\0';
            return std::filesystem::path(path.data()).parent_path();
        }
#elif defined(__APPLE__)
        std::uint32_t requiredLength = 0;
        (void)_NSGetExecutablePath(nullptr, &requiredLength);
        if (requiredLength > 0)
        {
            std::vector<char> path(requiredLength + 1, '\0');
            if (_NSGetExecutablePath(path.data(), &requiredLength) == 0)
            {
                return std::filesystem::path(path.data()).parent_path();
            }
        }
#endif

        std::error_code error;
        const std::filesystem::path currentDirectory = std::filesystem::current_path(error);
        return error ? std::filesystem::path(".") : currentDirectory;
    }

    bool HasExistingEditorData(const std::filesystem::path& applicationDirectory)
    {
        constexpr std::array<std::string_view, 3> dataDirectories = {
            "projects", "exports", "presets"
        };
        for (const std::string_view directory : dataDirectories)
        {
            std::error_code error;
            if (std::filesystem::exists(applicationDirectory / std::string(directory), error) && !error)
            {
                return true;
            }
        }
        return false;
    }

    std::filesystem::path UserDataDirectory(const std::filesystem::path& applicationDirectory)
    {
        // Preserve an existing portable/unpacked installation's data location.
        // New installations use a per-user writable directory.
        if (HasExistingEditorData(applicationDirectory))
        {
            return applicationDirectory;
        }

#ifdef _WIN32
        const DWORD requiredLength = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (requiredLength > 0)
        {
            std::vector<wchar_t> value(static_cast<std::size_t>(requiredLength) + 1, L'\0');
            const DWORD copiedLength = GetEnvironmentVariableW(L"LOCALAPPDATA",
                                                                value.data(),
                                                                static_cast<DWORD>(value.size()));
            if (copiedLength > 0 && static_cast<std::size_t>(copiedLength) < value.size())
            {
                return std::filesystem::path(value.data()) / "Weasel";
            }
        }
#elif defined(__APPLE__)
        if (const char* home = std::getenv("HOME"); home && *home)
        {
            return std::filesystem::path(home) / "Library" / "Application Support" / "Weasel";
        }
#else
        if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"); xdgDataHome && *xdgDataHome)
        {
            return std::filesystem::path(xdgDataHome) / "Weasel";
        }
        if (const char* home = std::getenv("HOME"); home && *home)
        {
            return std::filesystem::path(home) / ".local" / "share" / "Weasel";
        }
#endif

        // This is mainly for portable/unpacked builds where the executable
        // directory is deliberately writable, or for unusually restricted
        // environments with no usable user-data environment variable.
        return applicationDirectory;
    }

    std::string Lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    std::string Trim(std::string value)
    {
        const auto isNotWhitespace = [](unsigned char character)
        {
            return !std::isspace(character);
        };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), isNotWhitespace));
        value.erase(std::find_if(value.rbegin(), value.rend(), isNotWhitespace).base(), value.end());
        return value;
    }

    std::optional<std::string> SafeFilenameStem(const char* input, std::string_view extension, std::string& error)
    {
        std::string stem = Trim(input ? input : "");
        const std::string lower = Lowercase(stem);
        if (lower.ends_with(extension))
        {
            stem.resize(stem.size() - extension.size());
            stem = Trim(stem);
        }

        static constexpr std::string_view InvalidCharacters = "<>:\"/\\|?*";
        const bool hasInvalidCharacter = std::any_of(stem.begin(), stem.end(), [](unsigned char character)
        {
            return character < 32 || InvalidCharacters.find(static_cast<char>(character)) != std::string_view::npos;
        });
        if (stem.empty() || stem == "." || stem == ".." || stem.back() == '.' || hasInvalidCharacter)
        {
            error = "Use a simple filename without paths or special characters.";
            return std::nullopt;
        }

        error.clear();
        return stem;
    }

    std::string TimeText(double seconds)
    {
        seconds = std::max(0.0, seconds);
        const int wholeSeconds = static_cast<int>(seconds);
        const int minutes = wholeSeconds / 60;
        const int remainingSeconds = wholeSeconds % 60;
        const int centiseconds = static_cast<int>(std::floor((seconds - wholeSeconds) * 100.0 + 0.5));
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d.%02d", minutes, remainingSeconds, centiseconds % 100);
        return buffer;
    }

#if defined(__APPLE__) || defined(__linux__)
    std::string ShellQuote(const std::string& value)
    {
        std::string quoted = "'";
        for (const char character : value)
        {
            if (character == '\'')
            {
                quoted += "'\\''";
            }
            else
            {
                quoted += character;
            }
        }
        quoted += "'";
        return quoted;
    }

    std::string EscapeAppleScriptString(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char character : value)
        {
            if (character == '\\' || character == '\"')
            {
                escaped += '\\';
            }
            escaped += character;
        }
        return escaped;
    }

    std::string RunDesktopDialog(const std::string& command)
    {
        FILE* const pipe = ::popen(command.c_str(), "r");
        if (!pipe)
        {
            return {};
        }

        std::string output;
        std::array<char, 512> buffer{};
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        {
            output += buffer.data();
        }
        (void)::pclose(pipe);
        return output;
    }

    std::string TrimDesktopDialogOutput(std::string output)
    {
        while (!output.empty() && (output.back() == '\r' || output.back() == '\n'))
        {
            output.pop_back();
        }
        return output;
    }

    std::vector<std::filesystem::path> PathsFromDesktopDialog(const std::string& command, char separator)
    {
        const std::string output = TrimDesktopDialogOutput(RunDesktopDialog(command));
        std::vector<std::filesystem::path> paths;
        std::size_t start = 0;
        while (start <= output.size())
        {
            const std::size_t end = output.find(separator, start);
            const std::string path = TrimDesktopDialogOutput(output.substr(start, end - start));
            if (!path.empty())
            {
                paths.emplace_back(path);
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
        return paths;
    }
#endif

    std::vector<std::filesystem::path> ChooseMediaFiles(void* owner)
    {
#ifdef _WIN32
        std::array<wchar_t, 32768> fileBuffer{};
        const wchar_t filter[] =
            L"Media files\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.opus;*.wma;*.aif;*.aiff;*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;*.tiff\0"
            L"Video files\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v\0"
            L"Audio files\0*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.opus;*.wma;*.aif;*.aiff\0"
            L"Image files\0*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;*.tiff\0"
            L"All files\0*.*\0\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = static_cast<HWND>(owner);
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_PATHMUSTEXIST;
        dialog.lpstrTitle = L"Import media files";
        if (!GetOpenFileNameW(&dialog))
        {
            return {};
        }

        std::vector<std::filesystem::path> result;
        const std::wstring first = fileBuffer.data();
        const wchar_t* cursor = fileBuffer.data() + first.size() + 1;
        if (*cursor == L'\0')
        {
            result.emplace_back(first);
            return result;
        }

        while (*cursor != L'\0')
        {
            result.emplace_back(std::filesystem::path(first) / cursor);
            cursor += std::wcslen(cursor) + 1;
        }
        return result;
#else
        (void)owner;
#if defined(__APPLE__)
        const std::string command = "osascript -e " + ShellQuote(
            "set selectedFiles to choose file with prompt \"Import media\" with multiple selections allowed\n"
            "set selectedPaths to {}\n"
            "repeat with selectedFile in selectedFiles\n"
            "set end of selectedPaths to POSIX path of selectedFile\n"
            "end repeat\n"
            "set AppleScript's text item delimiters to \"|\"\n"
            "return selectedPaths as text");
#else
        const std::string command = "zenity --file-selection --multiple --separator='|' 2>/dev/null";
#endif
        return PathsFromDesktopDialog(command, '|');
#endif
    }

    std::optional<std::filesystem::path> ChooseCubeLutFile(
        void* owner,
        const std::filesystem::path& initialDirectory)
    {
#ifdef _WIN32
        std::array<wchar_t, 32768> fileBuffer{};
        const wchar_t filter[] = L"Cube LUT files\0*.cube\0\0";
        const std::wstring initialDirectoryWide = initialDirectory.wstring();
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = static_cast<HWND>(owner);
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
        dialog.lpstrInitialDir = initialDirectoryWide.empty() ? nullptr : initialDirectoryWide.c_str();
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        dialog.lpstrTitle = L"Choose a 3D LUT";
        if (!GetOpenFileNameW(&dialog))
        {
            return std::nullopt;
        }
        return std::filesystem::path(fileBuffer.data());
#else
        (void)owner;
        (void)initialDirectory;
#if defined(__APPLE__)
        const std::string command = "osascript -e " + ShellQuote(
            "POSIX path of (choose file with prompt \"Choose a 3D LUT\" of type {\"cube\"})");
#else
        const std::string command = "zenity --file-selection --file-filter='Cube LUT files | *.cube' 2>/dev/null";
#endif
        const std::string chosenPath = TrimDesktopDialogOutput(RunDesktopDialog(command));
        return chosenPath.empty() ? std::nullopt : std::optional<std::filesystem::path>(chosenPath);
#endif
    }

#ifndef _WIN32
    std::optional<std::filesystem::path> ChooseProjectFolder(
        void* owner,
        const char* title,
        const std::filesystem::path& initialDirectory = {})
    {
        (void)owner;
#if defined(__APPLE__)
        const std::string defaultLocation = initialDirectory.empty()
            ? ""
            : " default location POSIX file \"" + EscapeAppleScriptString(initialDirectory.string()) + "\"";
        const std::string command = "osascript -e " + ShellQuote(
            "POSIX path of (choose folder with prompt \""
            + EscapeAppleScriptString(title ? title : "Choose Weasel project folder") + "\"" + defaultLocation + ")");
#else
        const std::string command = "zenity --file-selection --directory --title="
            + ShellQuote(title ? title : "Choose Weasel project folder")
            + (initialDirectory.empty() ? "" : " --filename=" + ShellQuote(initialDirectory.string()))
            + " 2>/dev/null";
#endif
        const std::string chosenPath = TrimDesktopDialogOutput(RunDesktopDialog(command));
        return chosenPath.empty() ? std::nullopt : std::optional<std::filesystem::path>(chosenPath);
    }
#endif

    std::optional<std::filesystem::path> ChooseProjectDirectoryForOpen(void* owner)
    {
#ifdef _WIN32
        std::array<wchar_t, 32768> fileBuffer{};
        const wchar_t filter[] = L"Weasel project\0project.json\0\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = static_cast<HWND>(owner);
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        dialog.lpstrTitle = L"Open Weasel project";
        if (!GetOpenFileNameW(&dialog))
        {
            return std::nullopt;
        }
        return std::filesystem::path(fileBuffer.data()).parent_path();
#else
        return ChooseProjectFolder(owner, "Open Weasel project folder");
#endif
    }

    std::optional<std::filesystem::path> ChooseProjectDirectoryForSave(
        void* owner,
        const std::filesystem::path& initialDirectory,
        const std::string& suggestedName)
    {
#ifdef _WIN32
        std::array<wchar_t, 32768> fileBuffer{};
        const std::wstring suggestedNameWide(suggestedName.begin(), suggestedName.end());
        wcsncpy_s(fileBuffer.data(), fileBuffer.size(), suggestedNameWide.c_str(), _TRUNCATE);

        const wchar_t filter[] = L"Weasel project folder\0*.*\0\0";
        const std::wstring initialDirectoryWide = initialDirectory.wstring();
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = static_cast<HWND>(owner);
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
        dialog.lpstrInitialDir = initialDirectoryWide.empty() ? nullptr : initialDirectoryWide.c_str();
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST;
        dialog.lpstrTitle = L"Choose a Weasel project folder name";
        if (!GetSaveFileNameW(&dialog))
        {
            return std::nullopt;
        }
        return std::filesystem::path(fileBuffer.data());
#else
        const std::optional<std::filesystem::path> parentDirectory = ChooseProjectFolder(
            owner, "Choose where to create the project folder", initialDirectory);
        return parentDirectory ? std::optional<std::filesystem::path>(*parentDirectory / suggestedName) : std::nullopt;
#endif
    }

    std::optional<std::filesystem::path> ChooseExportFile(void* owner,
                                                           const std::filesystem::path& initialDirectory,
                                                           const std::string& suggestedName)
    {
#ifdef _WIN32
        std::array<wchar_t, 32768> fileBuffer{};
        const std::wstring suggestedNameWide(suggestedName.begin(), suggestedName.end());
        wcsncpy_s(fileBuffer.data(), fileBuffer.size(), suggestedNameWide.c_str(), _TRUNCATE);

        const wchar_t filter[] = L"MP4 video\0*.mp4\0All files\0*.*\0\0";
        const std::wstring initialDirectoryWide = initialDirectory.wstring();
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = static_cast<HWND>(owner);
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = fileBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
        dialog.lpstrInitialDir = initialDirectoryWide.empty() ? nullptr : initialDirectoryWide.c_str();
        dialog.lpstrDefExt = L"mp4";
        dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        dialog.lpstrTitle = L"Export video as";
        if (!GetSaveFileNameW(&dialog))
        {
            return std::nullopt;
        }

        std::filesystem::path destination(fileBuffer.data());
        // This editor currently exports MP4 containers. The dialog's default
        // extension covers ordinary saves; normalize an explicitly typed
        // alternative extension as well.
        if (Lowercase(destination.extension().string()) != ".mp4")
        {
            destination.replace_extension(".mp4");
        }
        return destination;
#else
        (void)owner;
#if defined(__APPLE__)
        const std::string escapedName = EscapeAppleScriptString(suggestedName);
        const std::string command = "osascript -e " + ShellQuote(
            "POSIX path of (choose file name with prompt \"Export video as\" default name \""
            + escapedName + "\")");
#else
        const std::filesystem::path defaultPath = initialDirectory / suggestedName;
        const std::string command = "zenity --file-selection --save --confirm-overwrite --filename="
            + ShellQuote(defaultPath.string()) + " 2>/dev/null";
#endif
        const std::string chosenPath = TrimDesktopDialogOutput(RunDesktopDialog(command));
        if (chosenPath.empty())
        {
            return std::nullopt;
        }
        std::filesystem::path destination(chosenPath);
        if (Lowercase(destination.extension().string()) != ".mp4")
        {
            destination.replace_extension(".mp4");
        }
        return destination;
#endif
    }

    void CopyToBuffer(std::array<char, 128>& buffer, const std::string& value)
    {
        std::fill(buffer.begin(), buffer.end(), '\0');
        const std::size_t length = std::min(value.size(), buffer.size() - 1);
        std::copy_n(value.begin(), static_cast<std::ptrdiff_t>(length), buffer.begin());
    }

    ImGuiWindowFlags FixedPanelFlags()
    {
        return ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoBringToFrontOnFocus;
    }

    ImTextureID ImGuiTextureId(GLuint textureHandle)
    {
        // ImGui 1.91 uses a pointer-shaped ImTextureID by default while 1.92
        // uses an integer one. Copy the OpenGL handle just as ImGui-SFML does
        // so the editor works with either representation.
        static_assert(sizeof(GLuint) <= sizeof(ImTextureID));
        ImTextureID textureId{};
        std::memcpy(&textureId, &textureHandle, sizeof(textureHandle));
        return textureId;
    }

    void DrawOpenGLTexture(GLuint textureHandle, const ImVec2& size)
    {
        const ImTextureID textureId = ImGuiTextureId(textureHandle);
#if IMGUI_VERSION_NUM >= 19200
        ImGui::Image(ImTextureRef(textureId), size);
#else
        ImGui::Image(textureId, size);
#endif
    }

    void DrawOpenGLTexture(ImDrawList& drawList,
                           GLuint textureHandle,
                           const ImVec2& minimum,
                           const ImVec2& maximum)
    {
        const ImTextureID textureId = ImGuiTextureId(textureHandle);
#if IMGUI_VERSION_NUM >= 19200
        drawList.AddImage(ImTextureRef(textureId), minimum, maximum);
#else
        drawList.AddImage(textureId, minimum, maximum);
#endif
    }

    void UploadPendingImGuiFontAtlasUpdates()
    {
#if IMGUI_VERSION_NUM >= 19200
        ImTextureData* const atlasTexture = ImGui::GetIO().Fonts->TexData;
        if (!atlasTexture || atlasTexture->Status != ImTextureStatus_WantUpdates
            || atlasTexture->Format != ImTextureFormat_RGBA32
            || atlasTexture->GetTexID() == ImTextureID_Invalid)
        {
            return;
        }

        // The ImGui 1.92 integration used by vcpkg and ImGui-SFML's 1.92
        // support branch hands SFML a sub-rectangle whose source rows still
        // use the full atlas pitch. Uploading the complete tightly packed
        // atlas avoids corrupting newly baked glyphs while preserving dynamic
        // font and Unicode support.
        GLint previousTexture = 0;
        GLint previousUnpackAlignment = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(atlasTexture->GetTexID()));
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        atlasTexture->Width,
                        atlasTexture->Height,
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        atlasTexture->GetPixels());
        glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
        atlasTexture->SetStatus(ImTextureStatus_OK);
#else
        // Stock ImGui-SFML 3.0 uses ImGui 1.91 and uploads the complete font
        // atlas during Init via UpdateFontTexture(). Its atlas does not expose
        // the 1.92 incremental texture-update interface, so rebuilding it
        // here would recreate and upload the texture every frame.
#endif
    }

}

namespace weasel
{
    Editor::Editor()
        : m_window(sf::VideoMode({ 1600, 960 }), "weasel")
        , m_applicationDirectory(ApplicationDirectory())
        , m_dataDirectory(UserDataDirectory(m_applicationDirectory))
        , m_recentProjects(m_dataDirectory / "recent_projects.json")
        , m_editorState()
        , m_project(m_editorState.project())
        , m_timelineController(m_editorState.timeline())
        , m_mediaImportController(m_applicationDirectory)
        , m_exportController(ExportController::ImGuiCallbacks{
            [this] { ApplyTheme(m_uiState.theme); },
            [] { UploadPendingImGuiFontAtlasUpdates(); }
        })
        , m_previewController(m_previewFrameCache)
        , m_mediaThumbnailController(m_previewFrameCache)
        , m_sequenceAudioController(m_applicationDirectory, m_editorState.cacheDirectory())
        , m_selectedAssetId(m_timelineController.selection().assetId)
    {
        m_imguiInitialized = ImGui::SFML::Init(m_window);
        if (!m_imguiInitialized)
        {
            m_running = false;
            return;
        }

        ApplyTheme(m_uiState.theme);
        std::error_code error;
        std::filesystem::create_directories(m_dataDirectory / "presets", error);
        std::string recentProjectsError;
        (void)m_recentProjects.load(recentProjectsError);
        CopyToBuffer(m_projectNameInput, "Untitled");
        CopyToBuffer(m_exportNameInput, m_project.exportSettings().outputFileName);
        updateWindowTitle();
        loadClipPresets();
        setupNativeFileDrop();
    }

    Editor::~Editor()
    {
        m_sequenceAudioController.reset();
        m_exportController.cancel();
        closeEncodingWindow(false);
        teardownNativeFileDrop();
        if (m_imguiInitialized)
        {
            ImGui::SFML::Shutdown();
        }
    }

    void Editor::run()
    {
        while (m_running && m_window.isOpen())
        {
            update();
        }
    }

    void Editor::update()
    {
        const sf::Time deltaTime = m_clock.restart();
        processEvents();
        if (!m_running || !m_window.isOpen())
        {
            if (!m_running && m_window.isOpen())
            {
                m_window.close();
            }
            return;
        }
        if (m_openEncodingWindowRequested)
        {
            m_openEncodingWindowRequested = false;
            if (!openEncodingWindow())
            {
                m_exportController.cancel();
            }
        }
        processEncodingEvents();
        processPendingProjectLoad();
        processPendingMediaImports();
        if (!m_pendingProjectLoad
            && m_pendingProjectAction == PendingProjectAction::None
            && !m_closeUnsavedChangesModalRequested)
        {
            processDroppedFiles();
        }
        if (!m_running || !m_window.isOpen())
        {
            return;
        }

        if (m_playing)
        {
            const double projectDuration = m_project.duration();
            m_project.sequence().playhead += deltaTime.asSeconds();
            if (m_project.sequence().playhead >= projectDuration)
            {
                m_project.sequence().playhead = 0.0;
                m_playing = false;
            }
        }

        ImGui::SFML::Update(m_window, deltaTime);
        // Keep the streaming audio clock serviced before synchronous preview
        // decode/compositing in renderUI can occupy the UI thread.
        if (!m_pendingProjectLoad)
        {
            updateSequenceAudio();
        }
        renderUI();
        updateWindowTitle();
        UploadPendingImGuiFontAtlasUpdates();
        m_window.clear(CurrentThemeBackgroundColour());
        ImGui::SFML::Render(m_window);
        m_window.display();
        if (!m_running)
        {
            m_window.close();
            return;
        }
        renderEncodingWindow(deltaTime);
    }

    void Editor::processEvents()
    {
        while (const std::optional event = m_window.pollEvent())
        {
            processEvent(*event);
        }
    }

    void Editor::processEvent(const sf::Event& event)
    {
        ImGui::SFML::ProcessEvent(m_window, event);
        if (event.is<sf::Event::Closed>())
        {
            requestProjectAction(PendingProjectAction::Quit);
            return;
        }

        if (const auto* key = event.getIf<sf::Event::KeyPressed>())
        {
            if (key->code == sf::Keyboard::Key::Escape)
            {
                if (m_pendingProjectAction != PendingProjectAction::None)
                {
                    clearPendingProjectAction();
                    m_closeUnsavedChangesModalRequested = true;
                }
                else if (!m_closeUnsavedChangesModalRequested)
                {
                    requestProjectAction(PendingProjectAction::Quit);
                }
                return;
            }
            if (m_pendingProjectAction != PendingProjectAction::None
                || m_closeUnsavedChangesModalRequested
                || m_pendingProjectLoad
                || !m_pendingMediaImports.empty())
            {
                return;
            }
            if (key->control && key->code == sf::Keyboard::Key::N)
            {
                requestProjectAction(PendingProjectAction::NewProject);
            }
            else if (key->control && key->shift && key->code == sf::Keyboard::Key::S)
            {
                saveProjectAsNamed();
            }
            else if (key->control && key->code == sf::Keyboard::Key::O)
            {
                requestProjectAction(PendingProjectAction::OpenProjectDialog);
            }
            else if (key->control && key->code == sf::Keyboard::Key::I)
            {
                importMediaDialog();
            }
            else if (key->control && key->code == sf::Keyboard::Key::S)
            {
                saveProject();
            }
            else if ((key->control || key->system) && !key->alt && !ImGui::GetIO().WantTextInput
                && key->code == sf::Keyboard::Key::Z)
            {
                if (key->shift)
                {
                    redoProject();
                }
                else
                {
                    undoProject();
                }
            }
            else if ((key->control || key->system) && !key->alt && !ImGui::GetIO().WantTextInput
                && key->code == sf::Keyboard::Key::Y)
            {
                redoProject();
            }
            else if ((key->control || key->system) && !key->alt && !ImGui::GetIO().WantTextInput
                && key->code == sf::Keyboard::Key::C)
            {
                copySelectedClip();
            }
            else if ((key->control || key->system) && !key->alt && !ImGui::GetIO().WantTextInput
                && key->code == sf::Keyboard::Key::V)
            {
                pasteCopiedClip();
            }
            else if ((key->control || key->system) && !key->alt && !ImGui::GetIO().WantTextInput
                && key->code == sf::Keyboard::Key::A)
            {
                if (m_activeProjectPanelTab == ProjectPanelTab::Media)
                {
                    if (m_timelineController.selectAllAssets())
                    {
                        resetPreview();
                    }
                }
                else
                {
                    endTimelineDrag();
                    if (m_timelineController.selectAllClips())
                    {
                        clearWaveformAlignment();
                    }
                }
            }
            else if (!key->control && !key->alt && !key->system && !ImGui::GetIO().WantTextInput
                && (key->code == sf::Keyboard::Key::Delete || key->code == sf::Keyboard::Key::Backspace))
            {
                if (hasSelectedMedia())
                {
                    deleteSelectedMedia();
                }
                else if (canCopySelectedClip())
                {
                    deleteSelectedClip();
                }
            }
            else if (!key->control && !key->alt && !key->system && !ImGui::GetIO().WantTextInput
                && key->code == sf::Keyboard::Key::S)
            {
                beginSequenceUndoTransaction();
                if (m_timelineController.splitSelectedClip(m_project.sequence().playhead))
                {
                    m_playing = false;
                    clearWaveformAlignment();
                    invalidatePreview();
                }
                commitSequenceUndoTransaction();
            }
            else if (!key->control && !key->alt && !key->system && !ImGui::GetIO().WantTextInput
                && key->code == sf::Keyboard::Key::C)
            {
                endTimelineDrag();
                m_uiState.timeline.tool = TimelineTool::Cut;
            }
            else if (!key->control && !key->alt && !key->system && !ImGui::GetIO().WantTextInput
                && key->code == sf::Keyboard::Key::V)
            {
                endTimelineDrag();
                m_uiState.timeline.tool = TimelineTool::Selection;
            }
            else if (!key->control && !key->alt && !key->system && !ImGui::GetIO().WantTextInput
                && key->code == sf::Keyboard::Key::Space)
            {
                const double duration = m_project.duration();
                if (duration > 0.0)
                {
                    if (!m_playing && m_project.sequence().playhead >= duration - 0.0001)
                    {
                        m_project.sequence().playhead = 0.0;
                    }
                    m_playing = !m_playing;
                }
            }
        }
    }

    void Editor::renderUI()
    {
        renderMenu();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 position = viewport->WorkPos;
        const ImVec2 size = viewport->WorkSize;

        float mediaWidth = 0.0f;
        float inspectorWidth = 0.0f;
        float monitorWidth = 0.0f;
        float timelineHeight = 0.0f;
        float upperHeight = 0.0f;
        const auto updateLayout = [&]()
        {
            if (m_mediaPanelWidth <= 0.0f)
            {
                m_mediaPanelWidth = std::clamp(size.x * 0.22f, 240.0f, 340.0f);
            }
            if (m_inspectorPanelWidth <= 0.0f)
            {
                m_inspectorPanelWidth = std::clamp(size.x * 0.20f, 255.0f, 330.0f);
            }
            if (m_timelinePanelHeight <= 0.0f)
            {
                m_timelinePanelHeight = std::clamp(size.y * 0.36f, 245.0f,
                    std::max(245.0f, size.y - MinimumUpperPanelHeight - PanelGap));
            }

            const float mediaMaximum = std::max(MinimumMediaPanelWidth,
                size.x - m_inspectorPanelWidth - MinimumMonitorWidth - PanelGap * 2.0f);
            m_mediaPanelWidth = std::clamp(m_mediaPanelWidth, MinimumMediaPanelWidth, mediaMaximum);

            const float inspectorMaximum = std::max(MinimumInspectorPanelWidth,
                size.x - m_mediaPanelWidth - MinimumMonitorWidth - PanelGap * 2.0f);
            m_inspectorPanelWidth = std::clamp(m_inspectorPanelWidth, MinimumInspectorPanelWidth, inspectorMaximum);

            const float timelineMaximum = std::max(MinimumTimelinePanelHeight,
                size.y - MinimumUpperPanelHeight - PanelGap);
            m_timelinePanelHeight = std::clamp(m_timelinePanelHeight,
                MinimumTimelinePanelHeight, timelineMaximum);

            mediaWidth = m_mediaPanelWidth;
            inspectorWidth = m_inspectorPanelWidth;
            monitorWidth = std::max(MinimumMonitorWidth,
                size.x - mediaWidth - inspectorWidth - PanelGap * 2.0f);
            timelineHeight = m_timelinePanelHeight;
            upperHeight = std::max(MinimumUpperPanelHeight, size.y - timelineHeight - PanelGap);
        };

        updateLayout();

        ImVec2 mediaMonitorSplitterMin(position.x + mediaWidth, position.y);
        ImVec2 mediaMonitorSplitterMax(mediaMonitorSplitterMin.x + PanelGap, position.y + upperHeight);
        ImVec2 monitorInspectorSplitterMin(position.x + mediaWidth + PanelGap + monitorWidth, position.y);
        ImVec2 monitorInspectorSplitterMax(monitorInspectorSplitterMin.x + PanelGap, position.y + upperHeight);
        ImVec2 upperTimelineSplitterMin(position.x, position.y + upperHeight);
        ImVec2 upperTimelineSplitterMax(position.x + size.x, upperTimelineSplitterMin.y + PanelGap);

        const auto mouseOverSplitter = [](const ImVec2& minimum, const ImVec2& maximum)
        {
            return ImGui::IsMouseHoveringRect(minimum, maximum, false);
        };

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_activeLayoutSplitter = LayoutSplitter::None;
        }
        else if (m_activeLayoutSplitter == LayoutSplitter::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (mouseOverSplitter(mediaMonitorSplitterMin, mediaMonitorSplitterMax))
            {
                m_activeLayoutSplitter = LayoutSplitter::MediaMonitor;
            }
            else if (mouseOverSplitter(monitorInspectorSplitterMin, monitorInspectorSplitterMax))
            {
                m_activeLayoutSplitter = LayoutSplitter::MonitorInspector;
            }
            else if (mouseOverSplitter(upperTimelineSplitterMin, upperTimelineSplitterMax))
            {
                m_activeLayoutSplitter = LayoutSplitter::UpperTimeline;
            }
        }

        if (m_activeLayoutSplitter != LayoutSplitter::None)
        {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            switch (m_activeLayoutSplitter)
            {
            case LayoutSplitter::MediaMonitor:
                m_mediaPanelWidth += delta.x;
                break;
            case LayoutSplitter::MonitorInspector:
                m_inspectorPanelWidth -= delta.x;
                break;
            case LayoutSplitter::UpperTimeline:
                m_timelinePanelHeight -= delta.y;
                break;
            case LayoutSplitter::None:
                break;
            }
            updateLayout();

            mediaMonitorSplitterMin = ImVec2(position.x + mediaWidth, position.y);
            mediaMonitorSplitterMax = ImVec2(mediaMonitorSplitterMin.x + PanelGap, position.y + upperHeight);
            monitorInspectorSplitterMin = ImVec2(position.x + mediaWidth + PanelGap + monitorWidth, position.y);
            monitorInspectorSplitterMax = ImVec2(monitorInspectorSplitterMin.x + PanelGap, position.y + upperHeight);
            upperTimelineSplitterMin = ImVec2(position.x, position.y + upperHeight);
            upperTimelineSplitterMax = ImVec2(position.x + size.x, upperTimelineSplitterMin.y + PanelGap);
        }

        renderProjectPanel(position, ImVec2(mediaWidth, upperHeight));
        renderVideoPreview(ImVec2(position.x + mediaWidth + PanelGap, position.y), ImVec2(monitorWidth, upperHeight));
        renderInspector(ImVec2(position.x + mediaWidth + monitorWidth + PanelGap * 2.0f, position.y), ImVec2(inspectorWidth, upperHeight));
        renderTimeline(ImVec2(position.x, position.y + upperHeight + PanelGap), ImVec2(size.x, timelineHeight));

        const auto drawSplitter = [&](LayoutSplitter splitter, const ImVec2& minimum, const ImVec2& maximum, ImGuiMouseCursor cursor)
        {
            const bool active = m_activeLayoutSplitter == splitter;
            const bool hovered = mouseOverSplitter(minimum, maximum);
            if (active || hovered)
            {
                ImGui::SetMouseCursor(cursor);
            }

            const ImU32 colour = active ? IM_COL32(86, 165, 242, 220)
                : (hovered ? IM_COL32(86, 165, 242, 130) : IM_COL32(75, 82, 96, 90));
            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            if (cursor == ImGuiMouseCursor_ResizeEW)
            {
                const float x = (minimum.x + maximum.x) * 0.5f;
                drawList->AddRectFilled(ImVec2(x - 1.0f, minimum.y), ImVec2(x + 1.0f, maximum.y), colour);
            }
            else
            {
                const float y = (minimum.y + maximum.y) * 0.5f;
                drawList->AddRectFilled(ImVec2(minimum.x, y - 1.0f), ImVec2(maximum.x, y + 1.0f), colour);
            }
        };

        drawSplitter(LayoutSplitter::MediaMonitor, mediaMonitorSplitterMin, mediaMonitorSplitterMax, ImGuiMouseCursor_ResizeEW);
        drawSplitter(LayoutSplitter::MonitorInspector, monitorInspectorSplitterMin, monitorInspectorSplitterMax, ImGuiMouseCursor_ResizeEW);
        drawSplitter(LayoutSplitter::UpperTimeline, upperTimelineSplitterMin, upperTimelineSplitterMax, ImGuiMouseCursor_ResizeNS);
        renderUnsavedChangesModal();
        renderOperationProgressModal();
    }

    void Editor::renderUnsavedChangesModal()
    {
        constexpr const char* ModalName = "Unsaved Changes";
        if (m_openUnsavedChangesModalRequested)
        {
            ImGui::OpenPopup(ModalName);
            m_openUnsavedChangesModalRequested = false;
        }

        if (!ImGui::IsPopupOpen(ModalName))
        {
            m_closeUnsavedChangesModalRequested = false;
            return;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(ModalName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            m_closeUnsavedChangesModalRequested = false;
            return;
        }

        if (m_closeUnsavedChangesModalRequested)
        {
            m_closeUnsavedChangesModalRequested = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        const char* actionDescription = "continuing";
        switch (m_pendingProjectAction)
        {
        case PendingProjectAction::NewProject:
            actionDescription = "starting a new project";
            break;
        case PendingProjectAction::OpenProjectDialog:
        case PendingProjectAction::OpenProjectFolder:
            actionDescription = "opening another project";
            break;
        case PendingProjectAction::Quit:
            actionDescription = "quitting";
            break;
        case PendingProjectAction::None:
            break;
        }

        ImGui::TextWrapped("This project has unsaved changes.");
        ImGui::Text("Save changes before %s?", actionDescription);
        if (!m_unsavedChangesError.empty())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", m_unsavedChangesError.c_str());
        }
        ImGui::Separator();

        if (ImGui::Button("Save", ImVec2(90.0f, 0.0f)))
        {
            if (saveProject())
            {
                ImGui::CloseCurrentPopup();
                executePendingProjectAction();
            }
            else
            {
                if (m_unsavedChangesError.empty())
                {
                    m_unsavedChangesError = "Could not save the project.";
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(90.0f, 0.0f)))
        {
            ImGui::CloseCurrentPopup();
            executePendingProjectAction();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)))
        {
            clearPendingProjectAction();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void Editor::renderOperationProgressModal()
    {
        constexpr const char* ModalName = "Operation Progress";
        const PendingProjectLoad* const projectLoad = m_pendingProjectLoad ? &*m_pendingProjectLoad : nullptr;
        const PendingMediaImport* const mediaImport = m_pendingMediaImports.empty()
            ? nullptr
            : &m_pendingMediaImports.front();

        if (!projectLoad && !mediaImport)
        {
            if (ImGui::BeginPopupModal(ModalName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            return;
        }

        ImGui::OpenPopup(ModalName);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(ModalName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }

        std::size_t completed = 0;
        std::size_t total = 0;
        std::string currentName;
        const char* title = "Importing media";
        const char* description = "Importing";
        if (projectLoad)
        {
            title = "Opening project";
            description = "Checking video display orientation for";
            completed = projectLoad->nextAsset;
            total = projectLoad->legacyVideoAssetIds.size();
            if (total > 0)
            {
                const std::size_t nameIndex = std::min(completed == 0 ? 0 : completed - 1, total - 1);
                if (const MediaAsset* asset = m_project.findAsset(projectLoad->legacyVideoAssetIds[nameIndex]))
                {
                    currentName = asset->name;
                }
            }
        }
        else
        {
            completed = mediaImport->nextPath;
            total = mediaImport->paths.size();
            if (total > 0)
            {
                const std::size_t nameIndex = std::min(completed == 0 ? 0 : completed - 1, total - 1);
                currentName = mediaImport->paths[nameIndex].filename().string();
            }
        }

        ImGui::TextUnformatted(title);
        if (!currentName.empty())
        {
            ImGui::TextWrapped("%s %s", description, currentName.c_str());
        }
        const float progress = total > 0
            ? static_cast<float>(completed) / static_cast<float>(total)
            : 0.0f;
        const std::string progressLabel = std::to_string(completed) + " of " + std::to_string(total);
        ImGui::ProgressBar(progress, ImVec2(360.0f, 0.0f), progressLabel.c_str());
        ImGui::EndPopup();
    }

    void Editor::requestProjectAction(PendingProjectAction action, std::filesystem::path projectDirectory)
    {
        if (action == PendingProjectAction::None
            || m_pendingProjectAction != PendingProjectAction::None
            || m_closeUnsavedChangesModalRequested
            || (action != PendingProjectAction::Quit
                && (m_pendingProjectLoad || !m_pendingMediaImports.empty())))
        {
            return;
        }

        // Timeline and inspector controls apply their values immediately but
        // intentionally coalesce history/dirty work until their current
        // transaction ends. Finish it before making any discard decision.
        commitPendingDocumentEdits();
        m_pendingProjectAction = action;
        m_pendingOpenProjectDirectory = std::move(projectDirectory);
        m_unsavedChangesError.clear();
        m_closeUnsavedChangesModalRequested = false;
        if (hasUnsavedChanges())
        {
            m_openUnsavedChangesModalRequested = true;
            return;
        }

        executePendingProjectAction();
    }

    void Editor::executePendingProjectAction()
    {
        const PendingProjectAction action = m_pendingProjectAction;
        if (action == PendingProjectAction::None)
        {
            return;
        }

        std::filesystem::path projectDirectory = std::move(m_pendingOpenProjectDirectory);
        clearPendingProjectAction();
        m_closeUnsavedChangesModalRequested = false;

        switch (action)
        {
        case PendingProjectAction::NewProject:
            startNewProject();
            break;
        case PendingProjectAction::OpenProjectDialog:
            openProjectDialog();
            break;
        case PendingProjectAction::OpenProjectFolder:
            openProject(projectDirectory);
            break;
        case PendingProjectAction::Quit:
            quitApplication();
            break;
        case PendingProjectAction::None:
            break;
        }
    }

    void Editor::clearPendingProjectAction()
    {
        m_pendingProjectAction = PendingProjectAction::None;
        m_pendingOpenProjectDirectory.clear();
        m_openUnsavedChangesModalRequested = false;
        m_unsavedChangesError.clear();
    }

    void Editor::clearWaveformAlignment() noexcept
    {
        m_uiState.waveformAlignment.clear();
    }

    void Editor::startNewProject()
    {
        m_editorState.resetNewProject();
        updateSequenceAudioCacheDirectory();
        m_mediaThumbnailController.reset();
        m_uiState.resetForProjectChange();
        CopyToBuffer(m_projectNameInput, "Untitled");
        CopyToBuffer(m_exportNameInput, m_project.exportSettings().outputFileName);
        clearWaveformAlignment();
        m_openProjectTab = true;
        m_openMediaTab = false;
        m_openExportTab = false;
        m_openSequenceTab = false;
        m_playing = false;
        resetPreview();
    }

    void Editor::quitApplication()
    {
        // A menu action or confirmation can request quit while ImGui is
        // building a frame. update() closes the window after that frame is
        // rendered so SFML is never asked to render a closed window.
        m_running = false;
    }

    void Editor::updateWindowTitle()
    {
        const std::string title = hasUnsavedChanges()
            ? "weasel [unsaved changes]"
            : "weasel";
        if (title != m_windowTitle)
        {
            m_window.setTitle(title);
            m_windowTitle = title;
        }
    }

    bool Editor::hasUnsavedChanges()
    {
        return m_editorState.isDirty() || m_timelineController.hasUncommittedChanges();
    }

    void Editor::renderMenu()
    {
        bool themeChanged = false;
        if (!ImGui::BeginMainMenuBar())
        {
            return;
        }

        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Project", "Ctrl+N"))
            {
                requestProjectAction(PendingProjectAction::NewProject);
            }
            if (ImGui::MenuItem("Open Project...", "Ctrl+O"))
            {
                requestProjectAction(PendingProjectAction::OpenProjectDialog);
            }
            if (ImGui::MenuItem("Save", "Ctrl+S"))
            {
                saveProject();
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
            {
                saveProjectAsNamed();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import Media...", "Ctrl+I"))
            {
                importMediaDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export..."))
            {
                m_openExportTab = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc"))
            {
                requestProjectAction(PendingProjectAction::Quit);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl/Cmd+Z", false, canUndoProject()))
            {
                undoProject();
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y / Cmd+Shift+Z", false, canRedoProject()))
            {
                redoProject();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Selected Clips", "Ctrl/Cmd+C", false, canCopySelectedClip()))
            {
                copySelectedClip();
            }
            if (ImGui::MenuItem("Paste Clips", "Ctrl/Cmd+V", false, canPasteClip()))
            {
                pasteCopiedClip();
            }
            const bool deletingSelectedMedia = hasSelectedMedia();
            const bool deletingSelectedClips = canCopySelectedClip();
            if (ImGui::MenuItem(deletingSelectedMedia ? "Delete Selected Media" : "Delete Selected Clips",
                                "Delete / Backspace",
                                false,
                                deletingSelectedMedia || deletingSelectedClips))
            {
                if (deletingSelectedMedia)
                {
                    deleteSelectedMedia();
                }
                else if (deletingSelectedClips)
                {
                    deleteSelectedClip();
                }
            }
            ImGui::EndMenu();
        }

        themeChanged = RenderThemesMenu(m_uiState.theme);

        ImGui::EndMainMenuBar();
        if (themeChanged)
        {
            ApplyTheme(m_uiState.theme);
            m_exportController.requestEncodingStyleRefresh();
        }
    }

    void Editor::renderProjectPanel(const ImVec2& position, const ImVec2& size)
    {
        ImGui::SetNextWindowPos(position, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::Begin("PROJECT", nullptr, FixedPanelFlags() | ImGuiWindowFlags_NoTitleBar);

        if (ImGui::BeginTabBar("ProjectTabs"))
        {
            const bool selectProjectTab = m_openProjectTab;
            const bool selectMediaTab = !selectProjectTab && m_openMediaTab;
            const bool selectExportTab = !selectProjectTab && !selectMediaTab && m_openExportTab;
            const bool selectSequenceTab = !selectProjectTab && !selectMediaTab && !selectExportTab && m_openSequenceTab;

            const ImGuiTabItemFlags projectTabFlags = selectProjectTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Project", nullptr, projectTabFlags))
            {
                m_activeProjectPanelTab = ProjectPanelTab::Project;
                m_openProjectTab = false;
                m_openSequenceTab = false;
                ImGui::Text("Project name (used by Save As)");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##projectfilename", m_projectNameInput.data(), m_projectNameInput.size());
                if (ImGui::Button("Save Project"))
                {
                    saveProject();
                }
                ImGui::SameLine();
                if (ImGui::Button("Save As..."))
                {
                    saveProjectAsNamed();
                }
                ImGui::SameLine();
                if (ImGui::Button("Open Project..."))
                {
                    requestProjectAction(PendingProjectAction::OpenProjectDialog);
                }
                ImGui::Separator();
                ImGui::Text("RECENT PROJECTS");
                if (ImGui::BeginChild("RecentProjectList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
                {
                    if (m_recentProjects.projects().empty())
                    {
                        ImGui::TextDisabled("No recent projects yet.");
                    }
                    for (const std::filesystem::path& projectDirectory : m_recentProjects.projects())
                    {
                        ImGui::PushID(projectDirectory.string().c_str());
                        const std::string displayName = projectDirectory.filename().string();
                        const std::string label = displayName.empty() ? projectDirectory.string() : displayName;
                        if (ImGui::Selectable(label.c_str(),
                                              projectDirectory == m_editorState.projectDirectory(),
                                              ImGuiSelectableFlags_AllowDoubleClick)
                            && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            requestProjectAction(PendingProjectAction::OpenProjectFolder, projectDirectory);
                        }
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s", projectDirectory.string().c_str());
                        }
                        ImGui::TextDisabled("%s", projectDirectory.parent_path().string().c_str());
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            const ImGuiTabItemFlags sequenceTabFlags = selectSequenceTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Sequence", nullptr, sequenceTabFlags))
            {
                m_activeProjectPanelTab = ProjectPanelTab::Sequence;
                m_openSequenceTab = false;
                m_sequenceInspector.render(*this);
                ImGui::EndTabItem();
            }

            const ImGuiTabItemFlags mediaTabFlags = selectMediaTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Media", nullptr, mediaTabFlags))
            {
                m_activeProjectPanelTab = ProjectPanelTab::Media;
                m_openMediaTab = false;
                m_openSequenceTab = false;
                const float importControlsHeight = ImGui::GetFrameHeightWithSpacing();
                const float mediaBinHeight = std::max(90.0f, ImGui::GetContentRegionAvail().y - importControlsHeight);

                const auto mediaFileAvailable = [](const MediaAsset& asset)
                {
                    std::error_code fileError;
                    return std::filesystem::is_regular_file(asset.path, fileError) && !fileError;
                };
                const auto activateMediaAsset = [this](const MediaAsset& asset)
                {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !io.KeyShift && !io.KeyCtrl)
                    {
                        (void)m_timelineController.selectAsset(asset.id);
                        resetPreview();

                        beginSequenceUndoTransaction();
                        if (TimelineClip* clip = addMediaToTimeline(asset.id, 0, m_project.duration()))
                        {
                            (void)m_timelineController.selectClip(clip->id);
                            m_openClipTab = true;
                        }
                        commitSequenceUndoTransaction();
                        return;
                    }

                    if (io.KeyShift)
                    {
                        (void)m_timelineController.selectAssetRange(asset.id);
                    }
                    else if (io.KeyCtrl)
                    {
                        (void)m_timelineController.toggleAssetSelection(asset.id);
                    }
                    else
                    {
                        (void)m_timelineController.selectAsset(asset.id);
                    }
                    resetPreview();
                };
                const auto renderMediaAssetDragSource = [](const MediaAsset& asset)
                {
                    if (!ImGui::BeginDragDropSource())
                    {
                        return;
                    }

                    ImGui::SetDragDropPayload("WEASEL_MEDIA_ASSET", &asset.id, sizeof(asset.id));
                    ImGui::Text("Add %s", asset.name.c_str());
                    ImGui::TextDisabled("Drop on a sequence track");
                    ImGui::EndDragDropSource();
                };
                const auto mediaDescription = [](const MediaAsset& asset)
                {
                    if (asset.isAudioOnly())
                    {
                        return std::string("Audio");
                    }
                    if (asset.isStillImage())
                    {
                        return std::string("Image  ") + std::to_string(asset.width)
                            + " x " + std::to_string(asset.height);
                    }
                    return std::string("Video  ") + std::to_string(asset.width)
                        + " x " + std::to_string(asset.height);
                };
                if (ImGui::BeginChild("MediaBin", ImVec2(0.0f, mediaBinHeight), ImGuiChildFlags_Borders))
                {
                    if (ImGui::BeginTabBar("MediaViewTabs"))
                    {
                        if (ImGui::BeginTabItem("List"))
                        {
                            if (m_project.assets().empty())
                            {
                                ImGui::TextDisabled("No media yet.");
                                ImGui::TextWrapped("Use Import Media to add files.");
                            }
                            else if (ImGui::BeginTable("MediaTable", 3,
                                                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
                            {
                                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.75f);
                                ImGui::TableSetupColumn("Media", ImGuiTableColumnFlags_WidthStretch, 0.85f);
                                ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthStretch, 0.70f);

                                for (const MediaAsset& asset : m_project.assets())
                                {
                                    ImGui::PushID(asset.id);
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);
                                    const bool fileAvailable = mediaFileAvailable(asset);
                                    if (!fileAvailable)
                                    {
                                        const ImVec4 unavailableColor(0.95f, 0.25f, 0.25f, 1.0f);
                                        ImGui::PushStyleColor(ImGuiCol_Text, unavailableColor);
                                        ImGui::PushStyleColor(ImGuiCol_TextDisabled, unavailableColor);
                                    }
                                    const bool selected = m_timelineController.isAssetSelected(asset.id);
                                    if (ImGui::Selectable(asset.name.c_str(), selected,
                                                          ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns))
                                    {
                                        activateMediaAsset(asset);
                                    }
                                    if (ImGui::IsItemHovered())
                                    {
                                        ImGui::SetTooltip("%s", asset.path.string().c_str());
                                    }
                                    renderMediaAssetDragSource(asset);

                                    ImGui::TableSetColumnIndex(1);
                                    ImGui::TextDisabled("%s", mediaDescription(asset).c_str());
                                    ImGui::TableSetColumnIndex(2);
                                    ImGui::TextDisabled("%s", TimeText(asset.duration).c_str());
                                    if (!fileAvailable)
                                    {
                                        ImGui::PopStyleColor(2);
                                    }
                                    ImGui::PopID();
                                }
                                ImGui::EndTable();
                            }
                            ImGui::EndTabItem();
                        }

                        if (ImGui::BeginTabItem("Icons"))
                        {
                            if (m_project.assets().empty())
                            {
                                ImGui::TextDisabled("No media yet.");
                                ImGui::TextWrapped("Use Import Media to add files.");
                            }
                            else
                            {
                                const ImGuiStyle& style = ImGui::GetStyle();
                                constexpr float targetCardWidth = 150.0f;
                                const float availableWidth = ImGui::GetContentRegionAvail().x;
                                const int columnCount = std::max(1, static_cast<int>((availableWidth + style.ItemSpacing.x)
                                    / (targetCardWidth + style.ItemSpacing.x)));
                                if (ImGui::BeginTable("MediaIconGrid", columnCount, ImGuiTableFlags_SizingStretchSame))
                                {
                                    for (const MediaAsset& asset : m_project.assets())
                                    {
                                        ImGui::PushID(asset.id);
                                        ImGui::TableNextColumn();

                                        const float cardWidth = ImGui::GetContentRegionAvail().x;
                                        const float cardPadding = style.FramePadding.x;
                                        const float thumbnailHeight = std::clamp(cardWidth * 0.5625f, 68.0f, 122.0f);
                                        const float cardHeight = thumbnailHeight + ImGui::GetTextLineHeight() * 2.0f
                                            + style.ItemSpacing.y * 2.0f + cardPadding * 4.0f;
                                        const ImVec2 cardPosition = ImGui::GetCursorScreenPos();
                                        const bool selected = m_timelineController.isAssetSelected(asset.id);
                                        const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
                                        ImGui::PushStyleColor(ImGuiCol_Header, transparent);
                                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
                                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
                                        const bool activated = ImGui::Selectable("##mediaIcon", selected,
                                            ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cardWidth, cardHeight));
                                        ImGui::PopStyleColor(3);

                                        const bool hovered = ImGui::IsItemHovered();
                                        const bool visible = ImGui::IsItemVisible();
                                        const ImVec2 cardMaximum = ImGui::GetItemRectMax();
                                        if (activated)
                                        {
                                            activateMediaAsset(asset);
                                        }
                                        if (hovered)
                                        {
                                            ImGui::SetTooltip("%s", asset.path.string().c_str());
                                        }
                                        renderMediaAssetDragSource(asset);

                                        const bool fileAvailable = mediaFileAvailable(asset);
                                        ImDrawList* const drawList = ImGui::GetWindowDrawList();
                                        const ImU32 cardColor = ImGui::GetColorU32(ImGuiCol_FrameBg);
                                        drawList->AddRectFilled(cardPosition, cardMaximum, cardColor, style.FrameRounding);
                                        if (selected || hovered)
                                        {
                                            ImVec4 highlight = ImGui::GetStyleColorVec4(selected
                                                ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered);
                                            highlight.w *= selected ? 0.45f : 0.25f;
                                            drawList->AddRectFilled(cardPosition, cardMaximum,
                                                ImGui::ColorConvertFloat4ToU32(highlight), style.FrameRounding);
                                        }

                                        const ImU32 borderColor = ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive : ImGuiCol_Border);
                                        drawList->AddRect(cardPosition, cardMaximum, borderColor, style.FrameRounding);

                                        const ImVec2 thumbnailMinimum(cardPosition.x + cardPadding, cardPosition.y + cardPadding);
                                        const ImVec2 thumbnailMaximum(cardMaximum.x - cardPadding,
                                            thumbnailMinimum.y + thumbnailHeight);
                                        drawList->AddRectFilled(thumbnailMinimum, thumbnailMaximum,
                                            ImGui::GetColorU32(ImGuiCol_ChildBg), style.FrameRounding);
                                        drawList->AddRect(thumbnailMinimum, thumbnailMaximum,
                                            ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);

                                        const MediaThumbnailController::ThumbnailResult thumbnailResult = fileAvailable
                                            && asset.isVisual() && visible
                                            ? m_mediaThumbnailController.request(asset)
                                            : MediaThumbnailController::ThumbnailResult{};
                                        if (thumbnailResult.texture)
                                        {
                                            const sf::Vector2u textureSize = thumbnailResult.texture->getSize();
                                            if (textureSize.x > 0 && textureSize.y > 0)
                                            {
                                                const float scale = std::min(
                                                    (thumbnailMaximum.x - thumbnailMinimum.x) / static_cast<float>(textureSize.x),
                                                    (thumbnailMaximum.y - thumbnailMinimum.y) / static_cast<float>(textureSize.y));
                                                const ImVec2 imageSize(static_cast<float>(textureSize.x) * scale,
                                                    static_cast<float>(textureSize.y) * scale);
                                                const ImVec2 imagePosition(
                                                    thumbnailMinimum.x + ((thumbnailMaximum.x - thumbnailMinimum.x) - imageSize.x) * 0.5f,
                                                    thumbnailMinimum.y + ((thumbnailMaximum.y - thumbnailMinimum.y) - imageSize.y) * 0.5f);
                                                DrawOpenGLTexture(*drawList,
                                                                  thumbnailResult.texture->getNativeHandle(),
                                                                  imagePosition,
                                                                  ImVec2(imagePosition.x + imageSize.x,
                                                                         imagePosition.y + imageSize.y));
                                            }
                                        }
                                        else
                                        {
                                            const char* placeholder = !fileAvailable ? "MISSING"
                                                : asset.isAudioOnly() ? "AUDIO"
                                                : thumbnailResult.failed ? "NO PREVIEW" : "Loading...";
                                            const ImVec2 placeholderSize = ImGui::CalcTextSize(placeholder);
                                            const ImVec2 placeholderPosition(
                                                thumbnailMinimum.x + ((thumbnailMaximum.x - thumbnailMinimum.x) - placeholderSize.x) * 0.5f,
                                                thumbnailMinimum.y + ((thumbnailMaximum.y - thumbnailMinimum.y) - placeholderSize.y) * 0.5f);
                                            const ImU32 placeholderColor = fileAvailable
                                                ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                                                : IM_COL32(242, 64, 64, 255);
                                            drawList->AddText(placeholderPosition, placeholderColor, placeholder);
                                        }

                                        const ImU32 textColor = fileAvailable ? ImGui::GetColorU32(ImGuiCol_Text)
                                            : IM_COL32(242, 64, 64, 255);
                                        const ImVec2 titlePosition(thumbnailMinimum.x, thumbnailMaximum.y + style.ItemSpacing.y);
                                        const ImVec2 textMaximum(cardMaximum.x - cardPadding,
                                            cardMaximum.y - cardPadding);
                                        drawList->PushClipRect(titlePosition, textMaximum, true);
                                        drawList->AddText(titlePosition, textColor, asset.name.c_str());
                                        const std::string details = mediaDescription(asset) + "  " + TimeText(asset.duration);
                                        drawList->AddText(ImVec2(titlePosition.x,
                                                                  titlePosition.y + ImGui::GetTextLineHeight()),
                                                          fileAvailable ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : textColor,
                                                          details.c_str());
                                        drawList->PopClipRect();

                                        ImGui::PopID();
                                    }
                                    ImGui::EndTable();
                                }
                            }
                            ImGui::EndTabItem();
                        }
                        ImGui::EndTabBar();
                    }
                }
                ImGui::EndChild();

                if (ImGui::Button("Import..."))
                {
                    importMediaDialog();
                }
                ImGui::EndTabItem();
            }

            const ImGuiTabItemFlags exportTabFlags = selectExportTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Export", nullptr, exportTabFlags))
            {
                m_activeProjectPanelTab = ProjectPanelTab::Export;
                m_openExportTab = false;
                m_openSequenceTab = false;
                ExportSettings editedExportSettings = m_project.exportSettings();
                bool exportSettingsChanged = false;
                const auto settingLabel = [](const char* label)
                {
                    // In the narrow left pane, ImGui's default right-side
                    // labels are squeezed by full-width widgets. Keep labels
                    // above their controls so both stay readable.
                    ImGui::TextUnformatted(label);
                    ImGui::SetNextItemWidth(-1.0f);
                };

                if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    settingLabel("Filename");
                    if (ImGui::InputText("##exportFilename", m_exportNameInput.data(), m_exportNameInput.size()))
                    {
                        editedExportSettings.outputFileName = m_exportNameInput.data();
                        exportSettingsChanged = true;
                    }
                }

                if (ImGui::CollapsingHeader("Video encoding", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::TextDisabled("Sequence: %d x %d  @ %.2f fps", m_project.sequence().width,
                                        m_project.sequence().height, m_project.sequence().fps);

                    const char* codecOptions[] = { "H.264 / AVC", "H.265 / HEVC" };
                    int codec = static_cast<int>(editedExportSettings.codec);
                    settingLabel("Codec");
                    if (ImGui::Combo("##exportCodec", &codec, codecOptions, 2))
                    {
                        editedExportSettings.codec = static_cast<ExportCodec>(codec);
                        exportSettingsChanged = true;
                    }

                    if (ImGui::Checkbox("Hardware encoding", &editedExportSettings.useGpuEncoding))
                    {
                        exportSettingsChanged = true;
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Uses an available hardware video encoder. The compositor always renders on the GPU.");
                    }

                    const char* presetOptions[] = { "Very fast", "Fast", "Medium", "Slow", "Very slow" };
                    int preset = static_cast<int>(editedExportSettings.preset);
                    settingLabel("Encoding preset");
                    if (ImGui::Combo("##exportPreset", &preset, presetOptions, 5))
                    {
                        editedExportSettings.preset = static_cast<ExportPreset>(preset);
                        exportSettingsChanged = true;
                    }

                    const char* rateControlOptions[] = { "Quality (CRF)", "Target bitrate" };
                    int rateControl = static_cast<int>(editedExportSettings.rateControl);
                    settingLabel("Rate control");
                    if (ImGui::Combo("##exportRateControl", &rateControl, rateControlOptions, 2))
                    {
                        editedExportSettings.rateControl = static_cast<ExportRateControl>(rateControl);
                        exportSettingsChanged = true;
                    }

                    bool hasEnabledVideoClip = false;
                    for (const TimelineTrack& track : m_project.sequence().tracks)
                    {
                        if (track.type != TimelineTrackType::Video || !track.enabled)
                        {
                            continue;
                        }
                        for (const TimelineClip& clip : track.clips)
                        {
                            const MediaAsset* asset = m_project.findAsset(clip.assetId);
                            if (asset && asset->kind == MediaKind::Video)
                            {
                                hasEnabledVideoClip = true;
                                break;
                            }
                        }
                        if (hasEnabledVideoClip)
                        {
                            break;
                        }
                    }
                    ImGui::BeginDisabled(!hasEnabledVideoClip);
                    if (ImGui::Button("Match Source"))
                    {
                        // Match Source can update cached asset metadata. Finish
                        // any inspector/timeline edit before that direct project
                        // mutation so it remains a separate undo step.
                        commitPendingDocumentEdits();
                        bool refreshedAssetMetadata = false;
                        const std::optional<int> sourceBitrate = maximumSequenceSourceVideoBitrateKbps(
                            refreshedAssetMetadata);
                        if (sourceBitrate)
                        {
                            const ExportRateControl previousRateControl = editedExportSettings.rateControl;
                            const int previousBitrate = editedExportSettings.videoBitrateKbps;
                            editedExportSettings.rateControl = ExportRateControl::TargetBitrate;
                            editedExportSettings.videoBitrateKbps = *sourceBitrate;
                            NormalizeExportSettings(editedExportSettings);
                            const int matchedBitrate = editedExportSettings.videoBitrateKbps;
                            exportSettingsChanged |= refreshedAssetMetadata
                                || previousRateControl != editedExportSettings.rateControl
                                || previousBitrate != matchedBitrate;
                        }
                    }
                    ImGui::EndDisabled();
                    if (!hasEnabledVideoClip)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("Add an enabled video clip to match its bitrate.");
                    }

                    if (editedExportSettings.rateControl == ExportRateControl::ConstantQuality)
                    {
                        settingLabel("Quality (CRF)");
                        if (ImGui::SliderInt("##exportQuality", &editedExportSettings.crf, 0, 51))
                        {
                            exportSettingsChanged = true;
                        }
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("Lower values produce higher quality and larger files. 18 is a high-quality default.");
                        }
                    }
                    else
                    {
                        settingLabel("Video bitrate (kb/s)");
                        if (ImGui::InputInt("##exportVideoBitrate", &editedExportSettings.videoBitrateKbps,
                                            0, 0))
                        {
                            exportSettingsChanged = true;
                        }
                    }
                }

                if (ImGui::CollapsingHeader("Audio encoding", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    const char* audioCodecOptions[] = { "AAC", "MP3" };
                    int audioCodec = static_cast<int>(editedExportSettings.audioCodec);
                    settingLabel("Audio codec");
                    if (ImGui::Combo("##exportAudioCodec", &audioCodec, audioCodecOptions, 2))
                    {
                        editedExportSettings.audioCodec = static_cast<AudioCodec>(audioCodec);
                        exportSettingsChanged = true;
                    }

                    const int maximumAudioBitrate = editedExportSettings.audioCodec == AudioCodec::Mp3 ? 320 : 512;
                    settingLabel("Audio bitrate");
                    if (ImGui::DragInt("##exportAudioBitrate", &editedExportSettings.audioBitrateKbps, 1.0f,
                                       64, maximumAudioBitrate, "%d kb/s"))
                    {
                        exportSettingsChanged = true;
                    }
                }

                ImGui::Separator();
                if (exportSettingsChanged)
                {
                    // Widgets edit a local copy so a pending timeline edit is
                    // committed before this independent project change.
                    commitPendingDocumentEdits();
                    m_project.exportSettings() = std::move(editedExportSettings);
                    NormalizeExportSettings(m_project.exportSettings());
                    m_editorState.recordChange();
                }
                const ExportStatus exportStatus = m_exportController.status();
                const bool exportRunning = exportStatus.state == ExportState::Running;
                const float exportButtonWidth = std::max(1.0f,
                    (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
                if (ImGui::Button("Export", ImVec2(exportButtonWidth, 0.0f)))
                {
                    if (exportRunning)
                    {
                        m_openEncodingWindowRequested = true;
                    }
                    else
                    {
                        startExport();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Export As...", ImVec2(-1.0f, 0.0f)))
                {
                    if (exportRunning)
                    {
                        m_openEncodingWindowRequested = true;
                    }
                    else
                    {
                        exportAsDialog();
                    }
                }

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void Editor::processEncodingEvents()
    {
        m_exportController.processEncodingEvents(m_window);
    }

    bool Editor::openEncodingWindow()
    {
        std::string error;
        return m_exportController.showEncodingWindow(m_window, error);
    }

    void Editor::closeEncodingWindow(bool cancelExport)
    {
        m_openEncodingWindowRequested = false;
        m_exportController.closeEncodingWindow(m_window, cancelExport);
    }

    void Editor::renderEncodingWindow(sf::Time deltaTime)
    {
        m_exportController.renderEncodingWindow(m_window, deltaTime);
    }
    void Editor::renderVideoPreview(const ImVec2& position, const ImVec2& size)
    {
        ImGui::SetNextWindowPos(position, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::Begin("VIDEO PREVIEW", nullptr, FixedPanelFlags() | ImGuiWindowFlags_NoTitleBar);

        // This is a sequence monitor: it always shows the composited sequence
        // canvas, including its black background and video-track compositing order.
        requestSequencePreview();

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const ImGuiStyle& style = ImGui::GetStyle();
        const float scrubberHeight = ImGui::GetFrameHeight() * 2.0f;
        const bool showAudioError = m_sequenceAudioController.playbackAudioEnabled()
            && !m_sequenceAudioController.error().empty();
        // The transport controls and monitor playhead are deliberately twice
        // the normal control height. Reserve that space before sizing the
        // preview canvas.
        const float controlsHeight = scrubberHeight + style.ItemSpacing.y
            + (showAudioError ? ImGui::GetTextLineHeightWithSpacing() : 0.0f) + 4.0f;
        const ImVec2 monitorSize(available.x, std::max(70.0f, available.y - controlsHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (ImGui::BeginChild("MonitorCanvas", monitorSize, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar))
        {
            const ImVec2 canvas = ImGui::GetContentRegionAvail();
            if (const sf::Texture* previewTexture = m_previewController.texture())
            {
                const sf::Vector2u textureSize = previewTexture->getSize();
                const float scale = std::min(canvas.x / static_cast<float>(textureSize.x), canvas.y / static_cast<float>(textureSize.y));
                const ImVec2 imageSize(static_cast<float>(textureSize.x) * scale, static_cast<float>(textureSize.y) * scale);
                ImGui::SetCursorPosX(std::max(0.0f, (canvas.x - imageSize.x) * 0.5f));
                ImGui::SetCursorPosY(std::max(0.0f, (canvas.y - imageSize.y) * 0.5f));
                // ImGui-SFML 3.0 as packaged by vcpkg reverses the background
                // and tint arguments of its sf::Texture Image overload. Pass
                // the native OpenGL texture to ImGui directly so the monitor
                // retains its normal transparent background and white tint.
                DrawOpenGLTexture(previewTexture->getNativeHandle(), imageSize);
            }
            else
            {
                const ImVec2 canvasPosition = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(canvasPosition, ImVec2(canvasPosition.x + canvas.x, canvasPosition.y + canvas.y), IM_COL32(0, 0, 0, 255));
                if (!m_previewController.error().empty())
                {
                    ImGui::SetCursorPos(ImVec2(12.0f, std::max(12.0f, canvas.y * 0.5f - ImGui::GetTextLineHeight())));
                    ImGui::TextDisabled("%s", m_previewController.error().c_str());
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        if (showAudioError)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Audio: %s", m_sequenceAudioController.error().c_str());
        }

        const float transportButtonWidth = std::max(scrubberHeight, ImGui::CalcTextSize("Pause").x + style.FramePadding.x * 2.0f);
        const float doubledScrubberPaddingY = std::max(style.FramePadding.y, (scrubberHeight - ImGui::GetFontSize()) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, doubledScrubberPaddingY));

        if (ImGui::Button(m_playing ? "Pause" : "Play", ImVec2(transportButtonWidth, scrubberHeight)))
        {
            if (m_project.duration() > 0.0)
            {
                m_playing = !m_playing;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("<", ImVec2(transportButtonWidth, scrubberHeight)))
        {
            const double frameDuration = 1.0 / std::max(1.0, m_project.sequence().fps);
            m_project.sequence().playhead = std::max(0.0, m_project.sequence().playhead - frameDuration);
            m_playing = false;
            requestScrubAudio();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Previous frame");
        }
        ImGui::SameLine();
        if (ImGui::Button(">", ImVec2(transportButtonWidth, scrubberHeight)))
        {
            const double frameDuration = 1.0 / std::max(1.0, m_project.sequence().fps);
            m_project.sequence().playhead = std::min(m_project.duration(), m_project.sequence().playhead + frameDuration);
            m_playing = false;
            requestScrubAudio();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Next frame");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        float playhead = static_cast<float>(m_project.sequence().playhead);
        if (ImGui::SliderFloat("##playhead", &playhead, 0.0f, std::max(0.05f, static_cast<float>(m_project.duration())), ""))
        {
            m_project.sequence().playhead = playhead;
            m_playing = false;
            requestScrubAudio();
        }
        const std::string playheadText = TimeText(m_project.sequence().playhead) + " / " + TimeText(m_project.duration());
        const ImVec2 playheadTextSize = ImGui::CalcTextSize(playheadText.c_str());
        const ImVec2 playheadMinimum = ImGui::GetItemRectMin();
        const ImVec2 playheadMaximum = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2((playheadMinimum.x + playheadMaximum.x - playheadTextSize.x) * 0.5f,
                   (playheadMinimum.y + playheadMaximum.y - playheadTextSize.y) * 0.5f),
            IM_COL32(235, 240, 248, 255),
            playheadText.c_str());
        ImGui::PopStyleVar();
        m_uiState.monitorPlayheadSliderActive = ImGui::IsItemActive();

        ImGui::End();
    }

    void Editor::renderInspector(const ImVec2& position, const ImVec2& size)
    {
        ImGui::SetNextWindowPos(position, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::Begin("INSPECTOR", nullptr, FixedPanelFlags() | ImGuiWindowFlags_NoTitleBar);

        if (ImGui::BeginTabBar("InspectorTabs"))
        {
            const bool selectClipTab = m_openClipTab;
            const ImGuiTabItemFlags videoTabFlags = selectClipTab
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            const bool videoTabOpen = ImGui::BeginTabItem("Video", nullptr, videoTabFlags);
            m_openClipTab = false;
            if (videoTabOpen)
            {
                m_clipInspector.render(*this, ClipInspector::View::Video);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Effects"))
            {
                m_clipInspector.render(*this, ClipInspector::View::Effects);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Audio"))
            {
                m_clipInspector.render(*this, ClipInspector::View::Audio);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        const bool inspectorItemActive = ImGui::IsAnyItemActive();
        ImGui::End();
        if (!inspectorItemActive && !m_timelineController.dragActive())
        {
            commitSequenceUndoTransaction();
        }
    }
    void Editor::renderTimeline(const ImVec2& position, const ImVec2& size)
    {
        m_timelineView.render(*this, position, size);
    }
    TimelineClip* Editor::addMediaToTimeline(int assetId, int trackIndex, double timelineStart)
    {
        bool sequenceWasEmpty = false;
        const bool ownsTransaction = !m_timelineController.transactionOpen();
        if (ownsTransaction)
        {
            beginSequenceUndoTransaction();
        }

        TimelineClip* added = m_project.addMediaToTimeline(
            assetId, trackIndex, timelineStart, &sequenceWasEmpty);
        if (!added)
        {
            if (ownsTransaction)
            {
                discardSequenceUndoTransaction();
            }
            return nullptr;
        }

        const int addedClipId = added->id;
        if (sequenceWasEmpty)
        {
            m_uiState.timeline.fitToSequence = true;
        }
        m_sequenceAudioController.invalidate();
        invalidatePreview();
        if (ownsTransaction)
        {
            (void)commitSequenceUndoTransaction();
        }
        return m_project.findClip(addedClipId);
    }

    void Editor::importMediaDialog()
    {
        queueMediaImport(ChooseMediaFiles(m_nativeWindow));
    }

    std::optional<std::filesystem::path> Editor::chooseCubeLutFile(
        const std::filesystem::path& currentLutPath)
    {
        const auto usableDirectory = [](const std::filesystem::path& directory)
        {
            std::error_code error;
            return !directory.empty() && std::filesystem::is_directory(directory, error) && !error;
        };

        std::filesystem::path initialDirectory;
        if (usableDirectory(currentLutPath.parent_path()))
        {
            initialDirectory = currentLutPath.parent_path();
        }
        else if (usableDirectory(m_editorState.projectDirectory()))
        {
            initialDirectory = m_editorState.projectDirectory();
        }
        else if (usableDirectory(m_dataDirectory))
        {
            initialDirectory = m_dataDirectory;
        }

        const std::optional<std::filesystem::path> selectedPath = ChooseCubeLutFile(
            m_nativeWindow, initialDirectory);
        if (!selectedPath)
        {
            return std::nullopt;
        }

        if (Lowercase(selectedPath->extension().string()) != ".cube")
        {
            return std::nullopt;
        }

        std::error_code filesystemError;
        if (!std::filesystem::is_regular_file(*selectedPath, filesystemError) || filesystemError)
        {
            return std::nullopt;
        }

        const std::filesystem::path absolutePath = std::filesystem::absolute(*selectedPath, filesystemError);
        return filesystemError ? selectedPath->lexically_normal() : absolutePath.lexically_normal();
    }

    bool Editor::importMedia(const std::filesystem::path& path)
    {
        commitPendingDocumentEdits();
        const MediaImportResult result = m_mediaImportController.importMedia(m_editorState.project(), path);
        if (!result.succeeded())
        {
            return false;
        }

        (void)m_timelineController.selectAsset(result.assetId);
        if (result.status == MediaImportStatus::AlreadyImported)
        {
            return true;
        }
        // MediaImportController intentionally works at the project-data boundary
        // and does not own editor dirty state. Record the successful asset
        // insertion here so import participates in unsaved-change prompts.
        if (result.addedToProject())
        {
            m_editorState.recordChange();
        }
        clearWaveformAlignment();
        m_openProjectTab = false;
        m_openMediaTab = true;
        m_openExportTab = false;
        m_openSequenceTab = false;
        resetPreview();
        return true;
    }

    void Editor::queueMediaImport(std::vector<std::filesystem::path> paths,
                                  std::optional<sf::Vector2i> dropPosition)
    {
        if (paths.empty())
        {
            return;
        }

        PendingMediaImport pending;
        pending.paths = std::move(paths);
        if (dropPosition)
        {
            pending.dropPosition = *dropPosition;
            pending.addToTimeline = true;
        }
        m_pendingMediaImports.push_back(std::move(pending));
    }

    void Editor::processPendingMediaImports()
    {
        if (m_pendingProjectLoad || m_pendingMediaImports.empty())
        {
            return;
        }

        PendingMediaImport& pending = m_pendingMediaImports.front();
        if (!pending.progressShown)
        {
            pending.progressShown = true;
            return;
        }

        if (pending.nextPath < pending.paths.size())
        {
            const std::filesystem::path path = pending.paths[pending.nextPath++];
            if (importMedia(path))
            {
                pending.importedAssetIds.push_back(m_selectedAssetId);
            }
        }

        if (pending.nextPath < pending.paths.size())
        {
            return;
        }

        if (pending.addToTimeline && !pending.importedAssetIds.empty())
        {
            m_pendingTimelineFileDrops.push_back({
                std::move(pending.importedAssetIds),
                ImVec2(static_cast<float>(pending.dropPosition.x),
                       static_cast<float>(pending.dropPosition.y))
            });
        }
        m_pendingMediaImports.pop_front();
    }

    bool Editor::processDroppedFiles()
    {
        std::vector<NativeFileDrop> droppedFiles;
        {
            std::lock_guard lock(m_droppedFilesMutex);
            droppedFiles.swap(m_droppedFiles);
        }
        bool queuedMedia = false;
        for (NativeFileDrop& droppedFile : droppedFiles)
        {
            if (droppedFile.paths.empty())
            {
                continue;
            }

            std::optional<sf::Vector2i> dropPosition;
            if (droppedFile.releasedInClientArea)
            {
                dropPosition = droppedFile.clientPosition;
            }
            queueMediaImport(std::move(droppedFile.paths), dropPosition);
            queuedMedia = true;
        }
        return queuedMedia;
    }

    void Editor::openProjectDialog()
    {
        const std::optional<std::filesystem::path> projectDirectory = ChooseProjectDirectoryForOpen(m_nativeWindow);
        if (projectDirectory)
        {
            openProject(*projectDirectory);
        }
    }

    bool Editor::openProject(const std::filesystem::path& projectDirectory)
    {
        if (m_pendingProjectLoad)
        {
            return false;
        }

        std::string error;
        if (!m_editorState.load(projectDirectory, error))
        {
            return false;
        }

        // Unlike the old synchronous open path, the display-orientation
        // refresh spans multiple UI frames. Stop old-project work before the
        // new project becomes visible behind its loading modal.
        updateSequenceAudioCacheDirectory();
        m_mediaThumbnailController.reset();
        m_sequenceAudioController.reset();
        clearWaveformAlignment();
        resetPreview();

        PendingProjectLoad pending;
        for (const MediaAsset& asset : m_project.assets())
        {
            if (asset.kind == MediaKind::Video && !asset.displayDimensionsKnown)
            {
                pending.legacyVideoAssetIds.push_back(asset.id);
            }
        }

        if (!pending.legacyVideoAssetIds.empty())
        {
            m_pendingProjectLoad = std::move(pending);
            return true;
        }

        finishOpenProject();
        return true;
    }

    void Editor::processPendingProjectLoad()
    {
        if (!m_pendingProjectLoad)
        {
            return;
        }

        PendingProjectLoad& pending = *m_pendingProjectLoad;
        if (!pending.progressShown)
        {
            pending.progressShown = true;
            return;
        }

        if (pending.nextAsset < pending.legacyVideoAssetIds.size())
        {
            const int assetId = pending.legacyVideoAssetIds[pending.nextAsset++];
            pending.projectChanged = m_mediaImportController.refreshLegacyVideoDisplayDimensions(m_project, assetId)
                || pending.projectChanged;
        }

        if (pending.nextAsset < pending.legacyVideoAssetIds.size())
        {
            return;
        }

        const bool projectChanged = pending.projectChanged;
        m_pendingProjectLoad.reset();
        if (projectChanged)
        {
            m_editorState.recordChange();
        }
        finishOpenProject();
    }

    void Editor::finishOpenProject()
    {
        updateSequenceAudioCacheDirectory();
        m_mediaThumbnailController.reset();
        m_uiState.resetForProjectChange();
        m_sequenceAudioController.reset();
        CopyToBuffer(m_projectNameInput, m_editorState.projectDirectory().filename().string());
        CopyToBuffer(m_exportNameInput, m_project.exportSettings().outputFileName);
        clearWaveformAlignment();
        m_openProjectTab = true;
        m_openMediaTab = false;
        m_openExportTab = false;
        m_openSequenceTab = false;
        m_playing = false;
        m_uiState.timeline.fitToSequence = true;
        resetPreview();
        rememberCurrentProject();
    }

    bool Editor::saveProject()
    {
        commitPendingDocumentEdits();
        return !m_editorState.hasProjectDirectory() ? saveProjectAsNamed() : [&]()
        {
            std::string error;
            if (!m_editorState.save(error))
            {
                if (m_pendingProjectAction != PendingProjectAction::None)
                {
                    m_unsavedChangesError = std::move(error);
                }
                return false;
            }
            updateSequenceAudioCacheDirectory();
            rememberCurrentProject();
            return true;
        }();
    }

    bool Editor::saveProjectAsNamed()
    {
        commitPendingDocumentEdits();
        std::string error;
        const std::optional<std::string> stem = SafeFilenameStem(m_projectNameInput.data(), ".json", error);
        if (!stem)
        {
            if (m_pendingProjectAction != PendingProjectAction::None)
            {
                m_unsavedChangesError = std::move(error);
            }
            return false;
        }
        const std::filesystem::path initialDirectory = m_editorState.hasProjectDirectory()
            ? m_editorState.projectDirectory().parent_path()
            : m_dataDirectory;
        const std::optional<std::filesystem::path> selectedDirectory = ChooseProjectDirectoryForSave(
            m_nativeWindow, initialDirectory, *stem);
        if (!selectedDirectory)
        {
            return false;
        }

        const std::string selectedDirectoryName = selectedDirectory->filename().string();
        const std::optional<std::string> selectedName = SafeFilenameStem(
            selectedDirectoryName.c_str(), ".json", error);
        if (!selectedName)
        {
            if (m_pendingProjectAction != PendingProjectAction::None)
            {
                m_unsavedChangesError = std::move(error);
            }
            return false;
        }

        const std::filesystem::path projectDirectory = selectedDirectory->parent_path() / *selectedName;
        if (!m_editorState.saveAs(projectDirectory, error, *selectedName))
        {
            if (m_pendingProjectAction != PendingProjectAction::None)
            {
                m_unsavedChangesError = std::move(error);
            }
            return false;
        }
        updateSequenceAudioCacheDirectory();
        CopyToBuffer(m_projectNameInput, *selectedName);
        rememberCurrentProject();
        return true;
    }

    void Editor::updateSequenceAudioCacheDirectory()
    {
        m_sequenceAudioController.setCacheDirectory(m_editorState.cacheDirectory());
    }

    void Editor::rememberCurrentProject()
    {
        std::string error;
        (void)m_recentProjects.remember(m_editorState.projectDirectory(), error);
    }

    void Editor::loadClipPresets()
    {
        m_clipInspector.resetPresetSelection();
        m_clipPresetLibrary.setFilePath(m_dataDirectory / "presets" / "clip_presets.json");
        std::string error;
        (void)m_clipPresetLibrary.load(error);
    }

    std::optional<int> Editor::maximumSequenceSourceVideoBitrateKbps(bool& refreshedAssetMetadata)
    {
        refreshedAssetMetadata = false;
        int maximumBitrateKbps = 0;
        std::unordered_set<int> inspectedAssetIds;
        for (const TimelineTrack& track : m_project.sequence().tracks)
        {
            // Mirror video export: disabled tracks and non-video rows do not
            // contribute picture to the rendered sequence.
            if (track.type != TimelineTrackType::Video || !track.enabled)
            {
                continue;
            }

            for (const TimelineClip& clip : track.clips)
            {
                if (!inspectedAssetIds.insert(clip.assetId).second)
                {
                    continue;
                }

                MediaAsset* asset = m_project.findAsset(clip.assetId);
                if (!asset || asset->kind != MediaKind::Video)
                {
                    continue;
                }

                // Projects saved before source bitrate metadata was added can
                // still use Match Source without being re-imported.
                if (asset->videoBitrateKbps <= 0)
                {
                    MediaAsset probedAsset;
                    std::string probeError;
                    if (MediaProbe::probe(asset->path,
                                          m_mediaImportController.ffprobePath(),
                                          probedAsset,
                                          probeError,
                                          MediaKind::Video)
                        && probedAsset.videoBitrateKbps > 0)
                    {
                        asset->videoBitrateKbps = probedAsset.videoBitrateKbps;
                        refreshedAssetMetadata = true;
                    }
                }

                maximumBitrateKbps = std::max(maximumBitrateKbps, asset->videoBitrateKbps);
            }
        }

        return maximumBitrateKbps > 0 ? std::optional<int>(maximumBitrateKbps) : std::nullopt;
    }

    void Editor::startExport(std::filesystem::path output)
    {
        std::string error;
        if (!output.empty())
        {
            if (Lowercase(output.extension().string()) != ".mp4")
            {
                output.replace_extension(".mp4");
            }
            CopyToBuffer(m_exportNameInput, output.filename().string());
        }
        // Export uses the live project immediately, so first finish any
        // coalesced inspector or timeline edit as its own history action.
        commitPendingDocumentEdits();
        const std::string outputFileName = m_exportNameInput.data();
        ExportSettings& exportSettings = m_project.exportSettings();
        if (exportSettings.outputFileName != outputFileName)
        {
            exportSettings.outputFileName = outputFileName;
            NormalizeExportSettings(exportSettings);
            m_editorState.recordChange();
        }
        const std::optional<std::string> stem = SafeFilenameStem(m_exportNameInput.data(), ".mp4", error);
        if (!stem)
        {
            return;
        }

        if (output.empty())
        {
            if (!m_editorState.hasProjectDirectory())
            {
                exportAsDialog();
                return;
            }
            output = m_editorState.exportDirectory() / (*stem + ".mp4");
        }

        if (m_exportController.startExport(m_window,
                                           m_editorState.project(),
                                           FindMediaTool(m_applicationDirectory, "ffmpeg"),
                                           output,
                                           error))
        {
            // Keep encoding activity separate from the editing panels.
            m_openExportTab = true;
            m_openEncodingWindowRequested = false;
        }
    }

    void Editor::exportAsDialog()
    {
        std::string error;
        const std::optional<std::string> stem = SafeFilenameStem(m_exportNameInput.data(), ".mp4", error);
        const std::string suggestedName = stem ? *stem + ".mp4" : "final_edit.mp4";
        std::filesystem::path initialDirectory = m_editorState.hasProjectDirectory()
            ? m_editorState.exportDirectory()
            : m_dataDirectory;
        if (!initialDirectory.empty())
        {
            std::error_code directoryError;
            std::filesystem::create_directories(initialDirectory, directoryError);
            if (directoryError && m_editorState.hasProjectDirectory())
            {
                initialDirectory = m_editorState.projectDirectory();
            }
        }
        const std::optional<std::filesystem::path> output = ChooseExportFile(
            m_nativeWindow, initialDirectory, suggestedName);
        if (output)
        {
            startExport(*output);
        }
    }

    void Editor::requestSequencePreview()
    {
        m_previewController.update(m_editorState.project(), m_playing,
                                   m_uiState.timeline.draggingPlayhead || m_uiState.monitorPlayheadSliderActive);
    }
    void Editor::updateSequenceAudio()
    {
        m_sequenceAudioController.update(m_editorState.project(),
                                         m_playing,
                                         m_uiState.timeline.draggingPlayhead || m_uiState.monitorPlayheadSliderActive);
    }

    void Editor::requestScrubAudio()
    {
        m_sequenceAudioController.requestScrub(m_editorState.project());
    }
    void Editor::invalidatePreview()
    {
        m_previewController.invalidate();
    }

    void Editor::resetPreview()
    {
        m_previewController.reset();
    }

    void Editor::beginSequenceUndoTransaction(bool timelineDrag)
    {
        m_timelineController.beginTransaction(timelineDrag);
    }

    bool Editor::commitSequenceUndoTransaction()
    {
        if (!m_timelineController.transactionOpen())
        {
            return false;
        }

        if (m_timelineController.commitTransaction())
        {
            m_sequenceAudioController.invalidate();
            invalidatePreview();
            return true;
        }
        return false;
    }

    void Editor::discardSequenceUndoTransaction()
    {
        const bool changed = m_timelineController.hasUncommittedChanges();
        m_timelineController.discardTransaction();
        if (changed)
        {
            m_sequenceAudioController.invalidate();
            invalidatePreview();
        }
    }

    void Editor::commitPendingDocumentEdits()
    {
        endTimelineDrag();
        commitSequenceUndoTransaction();
    }

    void Editor::afterTimelineHistoryRestore()
    {
        m_mediaThumbnailController.prune(m_project.assets());
        clearWaveformAlignment();
        m_playing = false;
        m_sequenceAudioController.invalidate();
        invalidatePreview();
    }

    bool Editor::canUndoProject() const
    {
        return m_editorState.canUndo();
    }

    bool Editor::canRedoProject() const
    {
        return m_editorState.canRedo();
    }

    void Editor::undoProject()
    {
        commitPendingDocumentEdits();
        if (m_editorState.undo())
        {
            afterTimelineHistoryRestore();
        }
    }

    void Editor::redoProject()
    {
        commitPendingDocumentEdits();
        if (m_editorState.redo())
        {
            afterTimelineHistoryRestore();
        }
    }

    bool Editor::canCopySelectedClip() const
    {
        return m_timelineController.canCopySelectedClip();
    }

    bool Editor::canPasteClip() const
    {
        return m_timelineController.hasClipboard();
    }

    bool Editor::hasSelectedMedia() const
    {
        return m_timelineController.selectedClipIds().empty()
            && !m_timelineController.selectedAssetIds().empty();
    }

    void Editor::copySelectedClip()
    {
        if (!m_timelineController.copySelectedClip())
        {
            return;
        }
    }

    bool Editor::pasteCopiedClip()
    {
        if (!m_timelineController.hasClipboard())
        {
            return false;
        }

        const double pasteTime = std::max(0.0, m_project.sequence().playhead);
        if (!m_timelineController.pasteClipboard(pasteTime))
        {
            return false;
        }

        clearWaveformAlignment();
        m_openClipTab = true;
        m_playing = false;
        invalidatePreview();
        return true;
    }
    void Editor::alignSelectedClipsByWaveform(int anchorClipId, int movingClipId)
    {
        const auto clearPendingAlignment = [this]()
        {
            clearWaveformAlignment();
        };

        // Keep extraction opt-in: this action becomes available only after
        // waveform drawing has been enabled in the Sequence tab.
        if (!m_sequenceAudioController.drawAudioWaveforms())
        {
            clearPendingAlignment();
            return;
        }

        const std::vector<int>& selectedClipIds = m_timelineController.selectedClipIds();
        if (selectedClipIds.size() != 2
            || selectedClipIds.front() != anchorClipId
            || selectedClipIds.back() != movingClipId)
        {
            clearPendingAlignment();
            return;
        }

        TimelineClip* anchorClip = m_project.findClip(anchorClipId);
        TimelineClip* movingClip = m_project.findClip(movingClipId);
        if (!anchorClip || !movingClip || anchorClipId == movingClipId
            || anchorClip->linkedClipId == movingClipId || movingClip->linkedClipId == anchorClipId)
        {
            clearPendingAlignment();
            return;
        }

        if (!anchorClip->isNormalSpeed() || !movingClip->isNormalSpeed())
        {
            clearPendingAlignment();
            return;
        }

        const MediaAsset* anchorAsset = m_project.findAsset(anchorClip->assetId);
        const MediaAsset* movingAsset = m_project.findAsset(movingClip->assetId);
        if (!anchorAsset || !movingAsset || !anchorAsset->hasAudio || !movingAsset->hasAudio)
        {
            clearPendingAlignment();
            return;
        }

        AudioWaveformSnapshot anchorSnapshot = m_sequenceAudioController.waveformSnapshot(anchorAsset->id);
        AudioWaveformSnapshot movingSnapshot = m_sequenceAudioController.waveformSnapshot(movingAsset->id);
        const auto queueMissingWaveform = [this](const MediaAsset& asset, const AudioWaveformSnapshot& snapshot)
        {
            if (snapshot.status.state != AudioWaveformState::Ready
                && snapshot.status.state != AudioWaveformState::Failed)
            {
                (void)m_sequenceAudioController.requestWaveform(m_editorState.project(), asset.id);
            }
        };

        const bool anchorReady = anchorSnapshot.status.state == AudioWaveformState::Ready && anchorSnapshot.waveform;
        const bool movingReady = movingSnapshot.status.state == AudioWaveformState::Ready && movingSnapshot.waveform;
        if (!anchorReady || !movingReady)
        {
            if (anchorSnapshot.status.state == AudioWaveformState::Failed
                || movingSnapshot.status.state == AudioWaveformState::Failed)
            {
                clearPendingAlignment();
                return;
            }
            queueMissingWaveform(*anchorAsset, anchorSnapshot);
            queueMissingWaveform(*movingAsset, movingSnapshot);
            m_uiState.waveformAlignment.anchorClipId = anchorClipId;
            m_uiState.waveformAlignment.movingClipId = movingClipId;
            return;
        }

        const std::optional<double> alignedTimelineStart = FindQuietnessAlignment(
            *anchorClip, *anchorSnapshot.waveform, *movingClip, *movingSnapshot.waveform);
        if (!alignedTimelineStart)
        {
            clearPendingAlignment();
            return;
        }

        int movingTrack = -1;
        beginSequenceUndoTransaction();
        if (!m_project.findClip(movingClipId, &movingTrack) || movingTrack < 0
            || !m_project.moveClip(movingClipId, movingTrack, *alignedTimelineStart))
        {
            commitSequenceUndoTransaction();
            clearPendingAlignment();
            return;
        }

        m_playing = false;
        invalidatePreview();
        commitSequenceUndoTransaction();
        clearPendingAlignment();
    }

    bool Editor::deleteSelectedClip()
    {
        if (!m_timelineController.deleteSelectedClip())
        {
            m_openSequenceTab = true;
            return false;
        }

        clearWaveformAlignment();
        m_openSequenceTab = true;
        m_playing = false;
        invalidatePreview();
        return true;
    }

    bool Editor::deleteSelectedMedia()
    {
        commitPendingDocumentEdits();
        if (!m_timelineController.deleteSelectedAsset())
        {
            return false;
        }

        m_mediaThumbnailController.prune(m_project.assets());
        clearWaveformAlignment();
        m_playing = false;
        m_sequenceAudioController.invalidate();
        invalidatePreview();
        return true;
    }

    void Editor::endTimelineDrag()
    {
        m_uiState.timeline.draggingPlayhead = false;
        if (m_timelineController.endDrag())
        {
            m_sequenceAudioController.invalidate();
            invalidatePreview();
        }
    }
    void Editor::setupNativeFileDrop()
    {
#ifdef _WIN32
        const HWND nativeWindow = reinterpret_cast<HWND>(m_window.getNativeHandle());
        m_nativeWindow = nativeWindow;
        if (!nativeWindow)
        {
            return;
        }

        DragAcceptFiles(nativeWindow, TRUE);
        SetLastError(0);
        const LONG_PTR previous = SetWindowLongPtrW(nativeWindow, GWLP_WNDPROC,
                                                     reinterpret_cast<LONG_PTR>(&Editor::fileDropWindowProc));
        if (previous != 0 || GetLastError() == 0)
        {
            m_previousWindowProc = reinterpret_cast<WNDPROC>(previous);
            s_fileDropTarget = this;
        }
#endif
    }

    void Editor::teardownNativeFileDrop()
    {
#ifdef _WIN32
        if (s_fileDropTarget == this)
        {
            s_fileDropTarget = nullptr;
        }
        if (m_nativeWindow)
        {
            const HWND nativeWindow = static_cast<HWND>(m_nativeWindow);
            DragAcceptFiles(nativeWindow, FALSE);
            if (m_previousWindowProc)
            {
                SetWindowLongPtrW(nativeWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_previousWindowProc));
            }
        }
        m_previousWindowProc = nullptr;
#endif
        m_nativeWindow = nullptr;
    }

#ifdef _WIN32
    LRESULT CALLBACK Editor::fileDropWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Editor* editor = s_fileDropTarget;
        if (message == WM_DROPFILES && editor)
        {
            const HDROP drop = reinterpret_cast<HDROP>(wParam);
            POINT dropPoint{};
            const bool releasedInClientArea = DragQueryPoint(drop, &dropPoint) != FALSE;
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::filesystem::path> paths;
            for (UINT index = 0; index < count; ++index)
            {
                const UINT required = DragQueryFileW(drop, index, nullptr, 0);
                if (required == 0)
                {
                    continue;
                }
                std::wstring path(required + 1, L'\0');
                DragQueryFileW(drop, index, path.data(), static_cast<UINT>(path.size()));
                path.resize(required);
                paths.emplace_back(path);
            }
            DragFinish(drop);

            if (!paths.empty())
            {
                std::lock_guard lock(editor->m_droppedFilesMutex);
                editor->m_droppedFiles.push_back({ std::move(paths),
                                                   sf::Vector2i(dropPoint.x, dropPoint.y),
                                                   releasedInClientArea });
            }
            return 0;
        }

        if (editor && editor->m_previousWindowProc)
        {
            return CallWindowProcW(editor->m_previousWindowProc, window, message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
#endif
}
