#include "OfflineRender.h"

namespace
{
// Streams the reader's whole file through `engine`, plus the latency flush. `sink`
// (optional) receives the latency-compensated output; `progress` gets 0..1 and
// returns false to cancel. Returns false when cancelled.
bool streamFile (juce::AudioFormatReader& reader, bt::Engine& engine,
                 const std::function<void (const juce::AudioBuffer<float>&, int, int)>& sink,
                 const std::function<bool (double)>& progress)
{
    const int numCh = (int) reader.numChannels;
    const juce::int64 length = reader.lengthInSamples;
    constexpr int block = 8192;
    juce::AudioBuffer<float> buffer (numCh, block);
    juce::int64 toSkip = engine.latencySamples(), readPos = 0, done = 0;

    while (done < length)
    {
        buffer.clear();
        const auto nRead = (int) juce::jlimit<juce::int64> (0, block, length - readPos);
        if (nRead > 0)
            reader.read (&buffer, 0, nRead, readPos, true, true);
        readPos += nRead; // past the end we keep feeding silence to flush the lookahead

        engine.process (buffer.getArrayOfWritePointers(), numCh, block);

        const auto start = (int) std::min<juce::int64> (toSkip, block);
        toSkip -= start;
        const auto count = (int) std::min<juce::int64> (block - start, length - done);
        if (count > 0)
        {
            if (sink)
                sink (buffer, start, count);
            done += count;
        }

        if (progress && ! progress ((double) done / (double) std::max<juce::int64> (1, length)))
            return false;
    }
    return true;
}
} // namespace

bt::Stats analyseFileStats (const juce::File& in, juce::AudioFormatManager& formats,
                            const std::function<bool (double)>& progress)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (in));
    if (reader == nullptr)
        return {};

    bt::Engine engine;
    engine.prepare (reader->sampleRate, (int) reader->numChannels);
    bt::Params p;
    p.algorithm = bt::Params::Classic; // detection only: skips the synthesis stage
    engine.setParams (p);

    bt::StatsCollector collector;
    engine.onDecision = [&] (int64_t, const bt::Engine::Frame& f) { collector.add (f); };
    if (! streamFile (*reader, engine, {}, progress))
        return {};
    return collector.result();
}

RenderResult renderFile (const juce::File& in, const juce::File& out, const bt::Params& params,
                         juce::AudioFormatManager& formats, const std::function<bool (double)>& progress)
{
    RenderResult result;
    bool cancelled = false;
    const bool twoPass = params.algorithm == bt::Params::Offline;
    auto report = [&] (double base, double span)
    {
        return [&, base, span] (double f)
        {
            cancelled = progress && ! progress (base + f * span);
            return ! cancelled;
        };
    };

    bt::Stats stats;
    if (twoPass)
    {
        stats = analyseFileStats (in, formats, report (0.0, 0.5));
        if (cancelled)
        {
            result.error = "Cancelled";
            return result;
        }
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (in));
    if (reader == nullptr)
    {
        result.error = "Can't read " + in.getFileName();
        return result;
    }

    const int numCh = (int) reader->numChannels;
    const double sr = reader->sampleRate;

    bt::Engine engine;
    engine.prepare (sr, numCh);
    engine.setParams (params);
    engine.setStats (stats);

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

    const bool finished = streamFile (*reader, engine,
                                      [&] (const juce::AudioBuffer<float>& b, int start, int count)
                                      { writer->writeFromAudioSampleBuffer (b, start, count); },
                                      report (twoPass ? 0.5 : 0.0, twoPass ? 0.5 : 1.0));
    if (! finished)
    {
        writer.reset();
        out.deleteFile();
        result.error = "Cancelled";
        return result;
    }

    result.ok = true;
    result.breaths = engine.breathCount();
    result.seconds = (double) reader->lengthInSamples / sr;
    return result;
}

StatsJob::StatsJob (juce::File f, std::function<void (const bt::Stats&)> done)
    : juce::Thread ("BreathTamer analysis"), file (std::move (f)), onDone (std::move (done))
{
}

StatsJob::~StatsJob()
{
    stopThread (10000);
}

void StatsJob::run()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const auto stats = analyseFileStats (file, formats, [this] (double) { return ! threadShouldExit(); });
    if (! threadShouldExit())
        onDone (stats);
}
