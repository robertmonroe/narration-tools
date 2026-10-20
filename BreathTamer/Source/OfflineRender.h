#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "BreathEngine.h"

struct RenderResult
{
    bool ok = false;
    juce::String error;
    int64_t breaths = 0;
    double seconds = 0.0;
};

// Whole-file statistics pass for the Offline algorithm. `progress` receives 0..1
// and returns false to cancel; a cancelled or unreadable file gives invalid Stats.
bt::Stats analyseFileStats (const juce::File& in, juce::AudioFormatManager& formats,
                            const std::function<bool (double)>& progress);

// Renders `in` through the breath engine into a 24-bit WAV at `out`, removing the
// engine latency so the result lines up sample-for-sample with the source. With
// the Offline algorithm the file is analysed first (two passes).
// `progress` receives 0..1 and returns false to cancel.
RenderResult renderFile (const juce::File& in, const juce::File& out, const bt::Params& params,
                         juce::AudioFormatManager& formats, const std::function<bool (double)>& progress);

// Background whole-file analysis, so the standalone preview can use Offline too.
class StatsJob : public juce::Thread
{
public:
    StatsJob (juce::File file, std::function<void (const bt::Stats&)> onDone);
    ~StatsJob() override;
    void run() override;

private:
    juce::File file;
    std::function<void (const bt::Stats&)> onDone;
};
