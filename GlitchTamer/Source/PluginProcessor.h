#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "GlitchEngine.h"

class GlitchTamerProcessor : public juce::AudioProcessor
{
public:
    GlitchTamerProcessor();
    ~GlitchTamerProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    gt::Params currentParams() const;
    int64_t glitchCount() const { return engine.glitchCountShared(); }
    bool isStandalone() const { return wrapperType == wrapperType_Standalone; }

    // Standalone file player: when a file is loaded it replaces the live input.
    bool loadFile (const juce::File& file);
    juce::File getLoadedFile() const { return loadedFile; }
    bool hasFile() const { return fileLoaded.load(); }
    void setPlaying (bool shouldPlay);
    bool isPlaying() const { return transport.isPlaying(); }
    void rewind() { transport.setPosition (0.0); }
    double positionSeconds() const { return transport.getCurrentPosition(); }
    double lengthSeconds() const { return transport.getLengthInSeconds(); }

    juce::AudioFormatManager formats;
    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    gt::Engine engine;
    std::atomic<float>* sensParam = nullptr;
    std::atomic<float>* widthParam = nullptr;
    std::atomic<float>* glitchesOnlyParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    juce::TimeSliceThread readAheadThread { "GlitchTamer file read-ahead" };
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::File loadedFile;
    std::atomic<bool> fileLoaded { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlitchTamerProcessor)
};
