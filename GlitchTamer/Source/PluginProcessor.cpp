#include "PluginProcessor.h"
#include "PluginEditor.h"

GlitchTamerProcessor::GlitchTamerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "GlitchTamer", createLayout())
{
    formats.registerBasicFormats();

    sensParam         = apvts.getRawParameterValue ("sens");
    widthParam        = apvts.getRawParameterValue ("width");
    glitchesOnlyParam = apvts.getRawParameterValue ("glitchesOnly");
    bypassParam       = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
}

GlitchTamerProcessor::~GlitchTamerProcessor()
{
    transport.setSource (nullptr);
    readAheadThread.stopThread (2000);
}

juce::AudioProcessorValueTreeState::ParameterLayout GlitchTamerProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sens", 1 }, "Sensitivity",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 70.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "width", 1 }, "Repair width",
                                                        NormalisableRange<float> (5.0f, 80.0f, 0.5f), 30.0f,
                                                        AudioParameterFloatAttributes().withLabel ("ms")));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "glitchesOnly", 1 }, "Output glitches only", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return { p.begin(), p.end() };
}

gt::Params GlitchTamerProcessor::currentParams() const
{
    gt::Params p;
    p.sensitivity  = sensParam->load();
    p.widthMs      = widthParam->load();
    p.glitchesOnly = glitchesOnlyParam->load() > 0.5f;
    p.bypass       = bypassParam->get();
    return p;
}

void GlitchTamerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, std::max (getTotalNumInputChannels(), getTotalNumOutputChannels()));
    setLatencySamples (engine.latencySamples());
    transport.prepareToPlay (samplesPerBlock, sampleRate);
}

void GlitchTamerProcessor::releaseResources()
{
    transport.releaseResources();
}

bool GlitchTamerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void GlitchTamerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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

bool GlitchTamerProcessor::loadFile (const juce::File& file)
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

void GlitchTamerProcessor::setPlaying (bool shouldPlay)
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

void GlitchTamerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void GlitchTamerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* GlitchTamerProcessor::createEditor()
{
    return new GlitchTamerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GlitchTamerProcessor();
}
