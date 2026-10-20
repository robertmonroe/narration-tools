#include "OfflineRender.h"

RenderResult renderFile (const juce::File& in, const juce::File& out, const rt::Params& params,
                         juce::AudioFormatManager& formats, const std::function<bool (double)>& progress)
{
    RenderResult result;

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (in));
    if (reader == nullptr)
    {
        result.error = "Can't read " + in.getFileName();
        return result;
    }

    const int numCh = (int) reader->numChannels;
    const double sr = reader->sampleRate;
    const juce::int64 length = reader->lengthInSamples;

    rt::Engine engine;
    engine.setParams (params);
    engine.prepare (sr, numCh);

    out.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (out);
    if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
    {
        result.error = "Can't write " + out.getFullPathName();
        return result;
    }

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (sr)
                                                   .withNumChannels (numCh)
                                                   .withBitsPerSample (24));
    if (writer == nullptr)
    {
        result.error = "Can't create WAV writer";
        return result;
    }

    constexpr int block = 8192;
    juce::AudioBuffer<float> buffer (numCh, block);
    juce::int64 written = 0;

    while (written < length)
    {
        const auto n = (int) juce::jmin<juce::int64> (block, length - written);
        buffer.clear();
        reader->read (&buffer, 0, n, written, true, true);
        engine.process (buffer.getArrayOfWritePointers(), numCh, n, written, true);
        writer->writeFromAudioSampleBuffer (buffer, 0, n);
        written += n;

        if (progress && ! progress ((double) written / (double) std::max<juce::int64> (1, length)))
        {
            writer.reset();
            out.deleteFile();
            result.error = "Cancelled";
            return result;
        }
    }

    result.ok = true;
    result.reading = engine.reading();
    return result;
}

namespace
{
juce::String part (const char* name, float db, rt::Verdict v)
{
    const juce::String value = db <= -199.0f ? juce::String ("silence") : juce::String (db, 1) + " dB";
    return juce::String (name) + " " + value + (v == rt::Verdict::pass ? " PASS" : v == rt::Verdict::fail ? " FAIL" : "");
}
} // namespace

juce::String describe (const rt::Reading& r)
{
    return part ("floor", r.floorDb, rt::floorVerdict (r)) + ", " + part ("RMS", r.rmsDb, rt::rmsVerdict (r)) + ", "
         + part ("peak", r.peakDb, rt::peakVerdict (r));
}

bool allPass (const rt::Reading& r)
{
    return rt::floorVerdict (r) == rt::Verdict::pass && rt::rmsVerdict (r) == rt::Verdict::pass
        && rt::peakVerdict (r) == rt::Verdict::pass;
}
