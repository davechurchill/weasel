#pragma once

#include "ProjectData.h"
#include "PreviewFrameCache.h"

#include <SFML/Graphics/Texture.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace weasel
{
    class VideoCompositor;

    // Interactive-only monitor preferences. They never affect export output.
    struct PreviewSettings
    {
        bool fastWhileScrubbing = true;
        bool fastAllTheTime = false;
        int  fastPreviewDivisor = 2;
    };

    // Uses the shared asynchronous frame cache and owns the composited texture
    // displayed by the program monitor. The caller supplies the project and
    // current playback interaction state; ImGui remains outside this class.
    class PreviewController
    {
    private:
        PreviewFrameCache&               m_frames;
        PreviewSettings                  m_settings;
        std::size_t                      m_signature = 0;
        std::unique_ptr<VideoCompositor> m_gpu;
        std::string                      m_error;
        bool                             m_hasRequestTime = false;
        double                           m_lastRequestTime = 0.0;

    public:
        explicit PreviewController(PreviewFrameCache& frames);
        ~PreviewController();

        PreviewController(const PreviewController&) = delete;
        PreviewController& operator=(const PreviewController&) = delete;

        // Queues any needed source frames and composites a new monitor image
        // when all active layers are available. Returns true if the visible
        // texture or monitor error changed during this call.
        bool update(const ProjectData& project, bool playing, bool activelyScrubbing);

        // Causes the next update to recompose the current frame while leaving
        // the last good monitor texture visible until then.
        void invalidate();

        // Clears the monitor, decode cache, and directional request state.
        void reset();

        const sf::Texture* texture() const;
        const std::string& error() const;

        PreviewSettings& settings();
        const PreviewSettings& settings() const;
    };
}
