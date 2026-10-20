#include "PluginProcessor.h"
#include "PluginEditor.h"

MouthTamerProcessor::MouthTamerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MouthTamer", createLayout())
{
    formats.registerBasicFormats();

    sensParam       = apvts.getRawParameterValue ("sens");
    skewParam       = apvts.getRawParameterValue ("skew");
    wideningParam   = apvts.getRawParameterValue ("widening");
    clicksOnlyParam = apvts.getRawParameterValue ("clicksOnly");
    bypassParam     = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
}

MouthTamerProcessor::~MouthTamerProcessor()
{
    transport.setSource (nullptr);
    readAheadThread.stopThread (2000);
}

juce::AudioProcessorValueTreeState::ParameterLayout MouthTamerProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sens", 1 }, "Sensitivity",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "skew", 1 }, "Frequency skew",
                                                        NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "widening", 1 }, "Click widening",
                                                        NormalisableRange<float> (0.0f, 5.0f, 0.1f), 3.0f,
                                                        AudioParameterFloatAttributes().withLabel ("ms")));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "clicksOnly", 1 }, "Output clicks only", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return { p.begin(), p.end() };
}

mt::Params MouthTamerProcessor::currentParams() const
{
    mt::Params p;
    p.sensitivity = sensParam->load();
    p.skew        = skewParam->load();
    p.wideningMs  = wideningParam->load();
    p.clicksOnly  = clicksOnlyParam->load() > 0.5f;
    p.bypass      = bypassParam->get();
    return p;
}

void MouthTamerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, std::max (getTotalNumInputChannels(), getTotalNumOutputChannels()));
    setLatencySamples (engine.latencySamples());
    transport.prepareToPlay (samplesPerBlock, sampleRate);
}

void MouthTamerProcessor::releaseResources()
{
    transport.releaseResources();
}

bool MouthTamerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void MouthTamerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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

bool MouthTamerProcessor::loadFile (const juce::File& file)
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

void MouthTamerProcessor::setPlaying (bool shouldPlay)
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

void MouthTamerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void MouthTamerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* MouthTamerProcessor::createEditor()
{
    return new MouthTamerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MouthTamerProcessor();
}

