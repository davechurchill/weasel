#include "WaveformAlignment.h"

#include "AudioWaveformCache.h"
#include "Sequence.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr double AudioAlignmentBinSeconds = 1.0 / 8.0;
    constexpr std::size_t MinimumAudioAlignmentBins = 16;
    constexpr std::size_t PreferredAudioAlignmentOverlapBins = 64;
    constexpr std::size_t MaximumAudioAlignmentCandidates = 8;
    constexpr std::size_t MaximumDescriptorOccurrences = 12;

    struct QuietnessRun
    {
        bool   quiet = false;
        int    start = 0;
        int    length = 0;
    };

    struct QuietnessTransition
    {
        int             sampleIndex = 0;
        std::uint32_t   descriptor = 0;
    };

    struct QuietnessPattern
    {
        std::vector<unsigned char>          quiet;
        std::vector<QuietnessTransition>    transitions;
    };

    double Percentile(std::vector<double> values, double fraction)
    {
        if (values.empty())
        {
            return 0.0;
        }
        const std::size_t index = std::min(values.size() - 1,
            static_cast<std::size_t>(std::lround(std::clamp(fraction, 0.0, 1.0)
                                                  * static_cast<double>(values.size() - 1))));
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
        return values[index];
    }

    std::vector<QuietnessRun> FindQuietnessRuns(const std::vector<unsigned char>& quiet)
    {
        std::vector<QuietnessRun> runs;
        if (quiet.empty())
        {
            return runs;
        }

        int start = 0;
        unsigned char state = quiet.front();
        for (std::size_t index = 1; index <= quiet.size(); ++index)
        {
            if (index == quiet.size() || quiet[index] != state)
            {
                runs.push_back({ state != 0, start, static_cast<int>(index) - start });
                if (index < quiet.size())
                {
                    start = static_cast<int>(index);
                    state = quiet[index];
                }
            }
        }
        return runs;
    }

    int QuietnessRunBucket(int length)
    {
        int bucket = 0;
        for (int value = std::max(1, length); value > 1 && bucket < 15; value = (value + 1) / 2)
        {
            ++bucket;
        }
        return bucket;
    }

    QuietnessPattern BuildQuietnessPattern(const weasel::TimelineClip& clip,
                                           const weasel::AudioWaveform& waveform)
    {
        QuietnessPattern pattern;
        if (waveform.peaks.empty() || waveform.durationSeconds <= 0.0)
        {
            return pattern;
        }

        const double sourceStart = std::clamp(clip.sourceIn, 0.0, waveform.durationSeconds);
        const double sourceEnd = std::clamp(clip.sourceOut, sourceStart, waveform.durationSeconds);
        if (sourceEnd - sourceStart < AudioAlignmentBinSeconds)
        {
            return pattern;
        }

        const std::size_t sampleCount = static_cast<std::size_t>(std::ceil(
            (sourceEnd - sourceStart) / AudioAlignmentBinSeconds));
        std::vector<double> loudness(sampleCount, 0.0);
        for (std::size_t index = 0; index < sampleCount; ++index)
        {
            const double sourceTime = std::min(sourceEnd - 0.000001,
                sourceStart + (static_cast<double>(index) + 0.5) * AudioAlignmentBinSeconds);
            const std::size_t peakIndex = std::min(waveform.peaks.size() - 1,
                static_cast<std::size_t>(std::floor(std::clamp(sourceTime / waveform.durationSeconds, 0.0, 1.0)
                                                     * static_cast<double>(waveform.peaks.size()))));
            const weasel::AudioWaveformPeak& peak = waveform.peaks[peakIndex];
            const double amplitude = std::max(std::abs(static_cast<double>(peak.minimum)),
                                              std::abs(static_cast<double>(peak.maximum)));
            loudness[index] = std::log1p(amplitude * 2000.0);
        }

        std::vector<double> smoothed(sampleCount, 0.0);
        for (std::size_t index = 0; index < sampleCount; ++index)
        {
            const std::size_t first = index == 0 ? 0 : index - 1;
            const std::size_t last = std::min(sampleCount - 1, index + 1);
            double sum = 0.0;
            for (std::size_t neighbour = first; neighbour <= last; ++neighbour)
            {
                sum += loudness[neighbour];
            }
            smoothed[index] = sum / static_cast<double>(last - first + 1);
        }

        const double low = Percentile(smoothed, 0.15);
        const double high = Percentile(smoothed, 0.85);
        if (high - low < 0.0001)
        {
            return pattern;
        }
        const double quietThreshold = low + (high - low) * 0.32;
        pattern.quiet.resize(sampleCount);
        for (std::size_t index = 0; index < sampleCount; ++index)
        {
            pattern.quiet[index] = smoothed[index] <= quietThreshold ? 1 : 0;
        }

        // Suppress microphone-noise flicker without losing the much longer
        // quiet passages and pauses that make useful alignment landmarks.
        for (int pass = 0; pass < 2; ++pass)
        {
            const std::vector<QuietnessRun> runs = FindQuietnessRuns(pattern.quiet);
            for (std::size_t index = 1; index + 1 < runs.size(); ++index)
            {
                const QuietnessRun& previous = runs[index - 1];
                const QuietnessRun& current = runs[index];
                const QuietnessRun& next = runs[index + 1];
                if (current.length <= 2 && previous.quiet == next.quiet)
                {
                    std::fill(pattern.quiet.begin() + current.start,
                              pattern.quiet.begin() + current.start + current.length,
                              previous.quiet ? 1 : 0);
                }
            }
        }

        const std::vector<QuietnessRun> runs = FindQuietnessRuns(pattern.quiet);
        for (std::size_t index = 1; index + 1 < runs.size(); ++index)
        {
            const QuietnessRun& previous = runs[index - 1];
            const QuietnessRun& current = runs[index];
            const QuietnessRun& next = runs[index + 1];
            const std::uint32_t descriptor = (current.quiet ? 1u : 0u)
                | (static_cast<std::uint32_t>(QuietnessRunBucket(previous.length)) << 1u)
                | (static_cast<std::uint32_t>(QuietnessRunBucket(current.length)) << 5u)
                | (static_cast<std::uint32_t>(QuietnessRunBucket(next.length)) << 9u);
            pattern.transitions.push_back({ current.start, descriptor });
        }
        return pattern;
    }

    double ScoreQuietnessOffset(const QuietnessPattern& anchor,
                                const QuietnessPattern& moving,
                                int offsetBins,
                                std::size_t minimumOverlap)
    {
        const int anchorCount = static_cast<int>(anchor.quiet.size());
        const int movingCount = static_cast<int>(moving.quiet.size());
        const int anchorFirst = std::max(0, offsetBins);
        const int anchorLast = std::min(anchorCount, movingCount + offsetBins);
        if (anchorLast - anchorFirst < static_cast<int>(minimumOverlap))
        {
            return -1.0;
        }

        std::size_t matches = 0;
        std::size_t anchorQuiet = 0;
        std::size_t movingQuiet = 0;
        for (int anchorIndex = anchorFirst; anchorIndex < anchorLast; ++anchorIndex)
        {
            const int movingIndex = anchorIndex - offsetBins;
            const bool anchorIsQuiet = anchor.quiet[static_cast<std::size_t>(anchorIndex)] != 0;
            const bool movingIsQuiet = moving.quiet[static_cast<std::size_t>(movingIndex)] != 0;
            matches += anchorIsQuiet == movingIsQuiet ? 1u : 0u;
            anchorQuiet += anchorIsQuiet ? 1u : 0u;
            movingQuiet += movingIsQuiet ? 1u : 0u;
        }

        std::size_t anchorTransitions = 0;
        std::size_t movingTransitions = 0;
        std::size_t matchingTransitions = 0;
        const int transitionFirst = std::max(anchorFirst + 1, offsetBins + 1);
        for (int anchorIndex = transitionFirst; anchorIndex < anchorLast; ++anchorIndex)
        {
            const int movingIndex = anchorIndex - offsetBins;
            const bool anchorTransition = anchor.quiet[static_cast<std::size_t>(anchorIndex)]
                != anchor.quiet[static_cast<std::size_t>(anchorIndex - 1)];
            const bool movingTransition = moving.quiet[static_cast<std::size_t>(movingIndex)]
                != moving.quiet[static_cast<std::size_t>(movingIndex - 1)];
            anchorTransitions += anchorTransition ? 1u : 0u;
            movingTransitions += movingTransition ? 1u : 0u;
            matchingTransitions += anchorTransition && movingTransition ? 1u : 0u;
        }
        if (matchingTransitions < 2 || anchorTransitions == 0 || movingTransitions == 0)
        {
            return -1.0;
        }

        const double overlap = static_cast<double>(anchorLast - anchorFirst);
        const double anchorQuietFraction = static_cast<double>(anchorQuiet) / overlap;
        const double movingQuietFraction = static_cast<double>(movingQuiet) / overlap;
        const double expectedAgreement = anchorQuietFraction * movingQuietFraction
            + (1.0 - anchorQuietFraction) * (1.0 - movingQuietFraction);
        const double agreement = static_cast<double>(matches) / overlap;
        const double quietnessScore = expectedAgreement < 0.999
            ? std::clamp((agreement - expectedAgreement) / (1.0 - expectedAgreement), -1.0, 1.0)
            : 0.0;
        const double transitionScore = 2.0 * static_cast<double>(matchingTransitions)
            / static_cast<double>(anchorTransitions + movingTransitions);
        return 0.35 * quietnessScore + 0.65 * transitionScore;
    }
}

namespace weasel
{
    std::optional<double> FindQuietnessAlignment(const TimelineClip& anchorClip,
                                                  const AudioWaveform& anchorWaveform,
                                                  const TimelineClip& movingClip,
                                                  const AudioWaveform& movingWaveform)
    {
        const QuietnessPattern anchor = BuildQuietnessPattern(anchorClip, anchorWaveform);
        const QuietnessPattern moving = BuildQuietnessPattern(movingClip, movingWaveform);
        if (anchor.quiet.size() < MinimumAudioAlignmentBins || moving.quiet.size() < MinimumAudioAlignmentBins
            || anchor.transitions.empty() || moving.transitions.empty())
        {
            return std::nullopt;
        }

        std::unordered_map<std::uint32_t, std::vector<int>> movingTransitions;
        for (const QuietnessTransition& transition : moving.transitions)
        {
            movingTransitions[transition.descriptor].push_back(transition.sampleIndex);
        }
        for (auto& entry : movingTransitions)
        {
            if (entry.second.size() > MaximumDescriptorOccurrences)
            {
                entry.second.clear();
            }
        }

        std::unordered_map<int, int> offsetVotes;
        for (const QuietnessTransition& transition : anchor.transitions)
        {
            const auto matches = movingTransitions.find(transition.descriptor);
            if (matches == movingTransitions.end())
            {
                continue;
            }
            for (const int movingIndex : matches->second)
            {
                ++offsetVotes[transition.sampleIndex - movingIndex];
            }
        }
        if (offsetVotes.empty())
        {
            return std::nullopt;
        }

        std::vector<std::pair<int, int>> rankedOffsets(offsetVotes.begin(), offsetVotes.end());
        std::sort(rankedOffsets.begin(), rankedOffsets.end(), [](const auto& left, const auto& right)
        {
            if (left.second != right.second)
            {
                return left.second > right.second;
            }
            return std::abs(left.first) < std::abs(right.first);
        });

        const std::size_t minimumOverlap = std::min({ PreferredAudioAlignmentOverlapBins,
                                                       anchor.quiet.size(), moving.quiet.size() });
        double bestScore = -1.0;
        double bestTimelineStart = 0.0;
        std::unordered_set<int> testedOffsets;
        const std::size_t candidateCount = std::min(MaximumAudioAlignmentCandidates, rankedOffsets.size());
        for (std::size_t candidate = 0; candidate < candidateCount; ++candidate)
        {
            for (int refinement = -3; refinement <= 3; ++refinement)
            {
                const int offsetBins = rankedOffsets[candidate].first + refinement;
                if (!testedOffsets.insert(offsetBins).second)
                {
                    continue;
                }
                const double newTimelineStart = anchorClip.timelineStart
                    + static_cast<double>(offsetBins) * AudioAlignmentBinSeconds;
                if (newTimelineStart < -0.0001)
                {
                    continue;
                }
                const double score = ScoreQuietnessOffset(anchor, moving, offsetBins, minimumOverlap);
                if (score > bestScore)
                {
                    bestScore = score;
                    bestTimelineStart = std::max(0.0, newTimelineStart);
                }
            }
        }

        return bestScore >= 0.24 ? std::optional<double>(bestTimelineStart) : std::nullopt;
    }
}
