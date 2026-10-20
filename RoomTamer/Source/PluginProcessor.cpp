#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
struct Preset
{
    const char* name;
    float levelDb, tone;
};

// Both sit safely under ACX's -60 dB noise floor limit.
constexpr Preset kPresets[] = {
    { "Audiobook (ACX)", -65.0f, 0.0f },
    { "Audiobook (ACX), extra quiet", -70.0f, 0.0f },
};
} // namespace

RoomTamerProcessor::RoomTamerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "RoomTamer", createLayout())
{
    formats.registerBasicFormats();

    levelParam  = apvts.getRawParameterValue ("level");
    toneParam   = apvts.getRawParameterValue ("tone");
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
}

RoomTamerProcessor::~RoomTamerProcessor()
{
    transport.setSource (nullptr);
    readAheadThread.stopThread (2000);
}

juce::AudioProcessorValueTreeState::ParameterLayout RoomTamerProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "level", 1 }, "Level",
                                                        NormalisableRange<float> (-90.0f, -50.0f, 0.1f), kPresets[0].levelDb,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "tone", 1 }, "Tone",
                                                        NormalisableRange<float> (-100.0f, 100.0f, 1.0f), kPresets[0].tone));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return { p.begin(), p.end() };
}

int RoomTamerProcessor::getNumPrograms()
{
    return (int) std::size (kPresets);
}

const juce::String RoomTamerProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, getNumPrograms()) ? juce::String (kPresets[index].name) : juce::String();
}

void RoomTamerProcessor::setCurrentProgram (int index)
{
    // Hosts re-send the current program when a project loads; only a real change
    // overwrites the saved Level and Tone.
    if (index != currentProgram)
        applyProgram (index);
}

void RoomTamerProcessor::applyProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumPrograms()))
        return;
    currentProgram = index;
    for (auto [id, value] : { std::pair { "level", kPresets[index].levelDb }, std::pair { "tone", kPresets[index].tone } })
        if (auto* param = apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
}

rt::Params RoomTamerProcessor::currentParams() const
{
    rt::Params p;
    p.levelDb = levelParam->load();
    p.tone    = toneParam->load();
    p.bypass  = bypassParam->get();
    return p;
}

void RoomTamerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.setParams (currentParams());
    engine.prepare (sampleRate, std::max (getTotalNumInputChannels(), getTotalNumOutputChannels()));
    setLatencySamples (0);
    transport.prepareToPlay (samplesPerBlock, sampleRate);
}

void RoomTamerProcessor::releaseResources()
{
    transport.releaseResources();
}

bool RoomTamerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void RoomTamerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    // In a host the room tone follows the timeline, so every render of a range
    // is identical; the meter counts only while the transport plays.
    bool playing = true;
    int64_t pos = -1;
    if (isStandalone())
    {
        if (fileLoaded.load())
        {
            buffer.clear();
            transport.getNextAudioBlock (juce::AudioSourceChannelInfo (buffer));
            playing = transport.isPlaying();
        }
    }
    else if (auto* head = getPlayHead())
    {
        if (const auto info = head->getPosition())
        {
            playing = info->getIsPlaying() || info->getIsRecording();
            if (playing)
                if (const auto t = info->getTimeInSamples())
                    pos = *t;
        }
    }

    if (playing && ! wasPlaying)
        engine.requestMeterReset();
    wasPlaying = playing;

    engine.setParams (currentParams());
    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), pos, playing);
}

bool RoomTamerProcessor::loadFile (const juce::File& file)
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
    return true;
}

void RoomTamerProcessor::setPlaying (bool shouldPlay)
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

void RoomTamerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("program", currentProgram, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void RoomTamerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            currentProgram = juce::jlimit (0, getNumPrograms() - 1, (int) state.getProperty ("program", 0));
            apvts.replaceState (state);
        }
}

juce::AudioProcessorEditor* RoomTamerProcessor::createEditor()
{
    return new RoomTamerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RoomTamerProcessor();
}
