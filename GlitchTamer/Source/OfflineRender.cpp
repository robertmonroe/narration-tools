#include "OfflineRender.h"

RenderResult renderFile (const juce::File& in, const juce::File& out, const gt::Params& params,
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

    gt::Engine engine;
    engine.prepare (sr, numCh);
    engine.setParams (params);

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
    juce::int64 toSkip = engine.latencySamples(), readPos = 0, written = 0;

    while (written < length)
    {
        buffer.clear();
        const auto nRead = (int) juce::jlimit<juce::int64> (0, block, length - readPos);
        if (nRead > 0)
            reader->read (&buffer, 0, nRead, readPos, true, true);
        readPos += nRead; // past the end we keep feeding silence to flush the lookahead

        engine.process (buffer.getArrayOfWritePointers(), numCh, block);

        const auto start = (int) std::min<juce::int64> (toSkip, block);
        toSkip -= start;
        const auto count = (int) std::min<juce::int64> (block - start, length - written);
        if (count > 0)
        {
            writer->writeFromAudioSampleBuffer (buffer, start, count);
            written += count;
        }

        if (progress && ! progress ((double) written / (double) std::max<juce::int64> (1, length)))
        {
            writer.reset();
            out.deleteFile();
            result.error = "Cancelled";
            return result;
        }
    }

    result.ok = true;
    result.glitches = engine.glitchCount();
    result.seconds = (double) length / sr;
    return result;
}
