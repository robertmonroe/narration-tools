#include "PluginProcessor.h"
#include "PluginEditor.h"

BreathTamerProcessor::BreathTamerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "BreathTamer", createLayout())
{
    formats.registerBasicFormats();

    modeParam        = apvts.getRawParameterValue ("mode");
    levelParam       = apvts.getRawParameterValue ("level");
    sensParam        = apvts.getRawParameterValue ("sens");
    ambienceParam    = apvts.getRawParameterValue ("ambience");
    algorithmParam   = apvts.getRawParameterValue ("algorithm");
    strictParam      = apvts.getRawParameterValue ("strictAmbience");
    breathsOnlyParam = apvts.getRawParameterValue ("breathsOnly");
    bypassParam      = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
}

BreathTamerProcessor::~BreathTamerProcessor()
{
    statsJob.reset();
    transport.setSource (nullptr);
    readAheadThread.stopThread (2000);
}

juce::AudioProcessorValueTreeState::ParameterLayout BreathTamerProcessor::createLayout()
{
    using namespace juce;
    const auto level = AudioParameterFloatAttributes()
                           .withLabel ("dB")
                           .withStringFromValueFunction ([] (float v, int)
                                                         { return v <= -99.95f ? String ("-inf") : String (v, 1); })
                           .withValueFromStringFunction ([] (const String& s)
                                                         { return s.trim().startsWithIgnoreCase ("-inf") ? -100.0f
                                                                                                          : s.getFloatValue(); });

    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Gain Mode",
                                                         StringArray { "Gain", "Target" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "level", 2 }, "Level",
                                                        NormalisableRange<float> (-100.0f, 0.0f, 0.1f), -20.0f, level));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sens", 1 }, "Detection Sensitivity",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 60.0f));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "ambience", 2 }, "Ambience",
                                                         StringArray { "Natural", "Gated" }, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "algorithm", 2 }, "Algorithm",
                                                         StringArray { "Real-time", "Offline", "Classic" }, 0));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "strictAmbience", 3 }, "Room tone fix", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "breathsOnly", 1 }, "Output breaths only", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return { p.begin(), p.end() };
}

bt::Params BreathTamerProcessor::currentParams() const
{
    bt::Params p;
    p.mode        = (int) modeParam->load();
    p.levelDb     = levelParam->load();
    p.sensitivity = sensParam->load();
    p.natural     = (int) ambienceParam->load() == 0;
    p.algorithm   = (int) algorithmParam->load();
    p.strictRoomTone = strictParam->load() > 0.5f;
    p.breathsOnly = breathsOnlyParam->load() > 0.5f;
    p.bypass      = bypassParam->get();
    return p;
}

void BreathTamerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, std::max (getTotalNumInputChannels(), getTotalNumOutputChannels()));
    setLatencySamples (engine.latencySamples());
    transport.prepareToPlay (samplesPerBlock, sampleRate);
}

void BreathTamerProcessor::releaseResources()
{
    transport.releaseResources();
}

bool BreathTamerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void BreathTamerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    if (isStandalone() && fileLoaded.load())
    {
        buffer.clear();
        transport.getNextAudioBlock (juce::AudioSourceChannelInfo (buffer));
    }

    bt::Stats stats;
    stats.valid    = isStandalone() && fileLoaded.load() && statsValid.load();
    stats.floorDb  = statsFloorDb.load();
    stats.speechDb = statsSpeechDb.load();

    engine.setParams (currentParams());
    engine.setStats (stats);
    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

bool BreathTamerProcessor::loadFile (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return false;

    if (! readAheadThread.isThreadRunning())
        readAheadThread.startThread();

    const double sourceRate = reader->sampleRate;
    auto source = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);

    transport.stop();
    transport.setSource (nullptr);
    transport.setSource (source.get(), 1 << 15, &readAheadThread, sourceRate, 2);
    readerSource = std::move (source);
    loadedFile = file;
    fileLoaded = true;

    // Whole-file analysis in the background so preview can use the Offline algorithm.
    statsJob.reset();
    statsValid = false;
    statsJob = std::make_unique<StatsJob> (file, [this] (const bt::Stats& s)
    {
        statsFloorDb = s.floorDb;
        statsSpeechDb = s.speechDb;
        statsValid = s.valid;
    });
    statsJob->startThread();
    return true;
}

void BreathTamerProcessor::setPlaying (bool shouldPlay)
{
    if (! fileLoaded.load())
        return;

    if (shouldPlay)
    {
        if (transport.hasStreamFinished())
            transport.setPosition (0.0);
        transport.start();
    }
    else
    {
        transport.stop();
    }
}

void BreathTamerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void BreathTamerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    // BreathTamer 1.0 stored separate "gain" and "target" values; carry the active one into "level".
    const auto tree = juce::ValueTree::fromXml (*xml);
    const auto param = [&tree] (const char* id) { return tree.getChildWithProperty ("id", id); };
    const bool legacy = ! param ("level").isValid() && param ("gain").isValid();
    const float legacyLevel = legacy ? (float) param ((float) param ("mode")["value"] > 0.5f ? "target" : "gain")["value"]
                                     : 0.0f;

    apvts.replaceState (tree);

    if (legacy)
        if (auto* p = apvts.getParameter ("level"))
            p->setValueNotifyingHost (p->convertTo0to1 (legacyLevel));
}

juce::AudioProcessorEditor* BreathTamerProcessor::createEditor()
{
    return new BreathTamerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BreathTamerProcessor();
}
