#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "RoomEngine.h"

class RoomTamerProcessor : public juce::AudioProcessor
{
public:
    RoomTamerProcessor();
    ~RoomTamerProcessor() override;

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

    // Factory presets (REAPER lists them in the FX preset menu).
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}
    void applyProgram (int index); // from the editor: always applies, even if already selected

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    rt::Params currentParams() const;
    rt::Reading reading() const { return engine.reading(); }
    void resetMeter() { engine.requestMeterReset(); }
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

    rt::Engine engine;
    std::atomic<float>* levelParam = nullptr;
    std::atomic<float>* toneParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;
    int currentProgram = 0;
    bool wasPlaying = false;

    juce::TimeSliceThread readAheadThread { "RoomTamer file read-ahead" };
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::File loadedFile;
    std::atomic<bool> fileLoaded { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RoomTamerProcessor)
};
