#include "PluginProcessor.h"
#include "PluginEditor.h"

PopTamerProcessor::PopTamerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PopTamer", createLayout())
{
    formats.registerBasicFormats();

    sensParam       = apvts.getRawParameterValue ("sens");
    strengthParam   = apvts.getRawParameterValue ("strength");
    limitParam      = apvts.getRawParameterValue ("limit");
    popsOnlyParam   = apvts.getRawParameterValue ("popsOnly");
    bypassParam     = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
}

PopTamerProcessor::~PopTamerProcessor()
{
    transport.setSource (nullptr);
    readAheadThread.stopThread (2000);
}

juce::AudioProcessorValueTreeState::ParameterLayout PopTamerProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sens", 1 }, "Sensitivity",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "strength", 1 }, "Strength",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 80.0f,
                                                        AudioParameterFloatAttributes().withLabel ("%")));
    NormalisableRange<float> limitRange (50.0f, 500.0f, 1.0f);
    limitRange.setSkewForCentre (150.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "limit", 1 }, "Frequency limit", limitRange, 100.0f,
                                                        AudioParameterFloatAttributes().withLabel ("Hz")));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "popsOnly", 1 }, "Output pops only", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return { p.begin(), p.end() };
}

pt::Params PopTamerProcessor::currentParams() const
{
    pt::Params p;
    p.sensitivity = sensParam->load();
    p.strength    = strengthParam->load();
    p.limitHz     = limitParam->load();
    p.popsOnly    = popsOnlyParam->load() > 0.5f;
    p.bypass      = bypassParam->get();
    return p;
}

void PopTamerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, std::max (getTotalNumInputChannels(), getTotalNumOutputChannels()));
    setLatencySamples (engine.latencySamples());
    transport.prepareToPlay (samplesPerBlock, sampleRate);
}

void PopTamerProcessor::releaseResources()
{
    transport.releaseResources();
}

bool PopTamerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void PopTamerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    if (isStandalone() && fileLoaded.load())
    {
        buffer.clear();
        transport.getNextAudioBlock (juce::AudioSourceChannelInfo (buffer));
    }

    engine.setParams (currentParams());
    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

bool PopTamerProcessor::loadFile (const juce::File& file)
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

void PopTamerProcessor::setPlaying (bool shouldPlay)
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

void PopTamerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void PopTamerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* PopTamerProcessor::createEditor()
{
    return new PopTamerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PopTamerProcessor();
}

