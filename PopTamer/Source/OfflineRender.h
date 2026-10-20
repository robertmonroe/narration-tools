#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "PopEngine.h"

struct RenderResult
{
    bool ok = false;
    juce::String error;
    int64_t pops = 0;
    double seconds = 0.0;
};

// Renders `in` through the pop engine into a 24-bit WAV at `out`, removing
// the engine latency so the result lines up sample-for-sample with the source.
// `progress` receives 0..1 and returns false to cancel.
RenderResult renderFile (const juce::File& in, const juce::File& out, const pt::Params& params,
                         juce::AudioFormatManager& formats, const std::function<bool (double)>& progress);
