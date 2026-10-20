#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "RoomEngine.h"

struct RenderResult
{
    bool ok = false;
    juce::String error;
    rt::Reading reading; // ACX check of the written file
};

// Renders `in` with room tone into a 24-bit WAV at `out` (no latency, same
// length). `progress` receives 0..1 and returns false to cancel.
RenderResult renderFile (const juce::File& in, const juce::File& out, const rt::Params& params,
                         juce::AudioFormatManager& formats, const std::function<bool (double)>& progress);

// One line for the status bar, e.g. "floor -65.8 dB PASS, RMS -20.1 dB PASS, peak -3.4 dB PASS".
juce::String describe (const rt::Reading& r);
bool allPass (const rt::Reading& r);
