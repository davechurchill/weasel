#include "EditHistory.h"

#include <algorithm>
#include <cmath>

namespace weasel
{
    void EditHistory::reset(const ProjectData& data)
    {
        m_states.assign(1, data);
        m_currentIndex = 0;
        m_savedIndex = 0;
    }

    bool EditHistory::record(const ProjectData& data)
    {
        if (m_currentIndex == NoIndex)
        {
            m_states.assign(1, data);
            m_currentIndex = 0;
            m_savedIndex = NoIndex;
            return true;
        }

        if (m_states[static_cast<std::size_t>(m_currentIndex)].sameContent(data))
        {
            return false;
        }

        if (m_currentIndex + 1 < static_cast<int>(m_states.size()))
        {
            if (m_savedIndex > m_currentIndex)
            {
                m_savedIndex = NoIndex;
            }
            m_states.erase(m_states.begin() + m_currentIndex + 1, m_states.end());
        }

        m_states.push_back(data);
        ++m_currentIndex;
        return true;
    }

    bool EditHistory::canUndo() const noexcept
    {
        return m_currentIndex > 0;
    }

    bool EditHistory::canRedo() const noexcept
    {
        return m_currentIndex != NoIndex
            && m_currentIndex + 1 < static_cast<int>(m_states.size());
    }

    bool EditHistory::undo(ProjectData& data)
    {
        if (!canUndo())
        {
            return false;
        }

        --m_currentIndex;
        return restore(data);
    }

    bool EditHistory::redo(ProjectData& data)
    {
        if (!canRedo())
        {
            return false;
        }

        ++m_currentIndex;
        return restore(data);
    }

    bool EditHistory::restore(ProjectData& data) const
    {
        if (m_currentIndex == NoIndex)
        {
            return false;
        }

        // Playhead movement is navigation, not an edit (sameContent ignores
        // it), so an undo should not unexpectedly jump the user elsewhere in
        // the timeline. Clamp it in case the restored sequence is shorter.
        const double currentPlayhead = data.sequence().playhead;
        data = m_states[static_cast<std::size_t>(m_currentIndex)];
        const double safePlayhead = std::isfinite(currentPlayhead)
            ? std::max(0.0, currentPlayhead)
            : 0.0;
        data.sequence().playhead = std::min(safePlayhead, data.duration());
        return true;
    }

    void EditHistory::markSaved(const ProjectData& data)
    {
        if (m_currentIndex == NoIndex)
        {
            reset(data);
            return;
        }

        m_states[static_cast<std::size_t>(m_currentIndex)] = data;
        m_savedIndex = m_currentIndex;
    }

    bool EditHistory::isDirty() const noexcept
    {
        return m_currentIndex != m_savedIndex;
    }

}
