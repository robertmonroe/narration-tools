#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "BreathEngine.h"
#include "OfflineRender.h"

class BreathTamerProcessor : public juce::AudioProcessor
{
public:
    BreathTamerProcessor();
    ~BreathTamerProcessor() override;

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

    bt::Params currentParams() const;
    float meterDb() const { return engine.meterDb(); }
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

    // Offline algorithm: whole-file statistics of the loaded file (standalone only).
    bool offlineStatsReady() const { return statsValid.load(); }
    bool isAnalysing() const { return statsJob != nullptr && statsJob->isThreadRunning(); }

    juce::AudioFormatManager formats;
    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    bt::Engine engine;
    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* levelParam = nullptr;
    std::atomic<float>* sensParam = nullptr;
    std::atomic<float>* ambienceParam = nullptr;
    std::atomic<float>* algorithmParam = nullptr;
    std::atomic<float>* strictParam = nullptr;
    std::atomic<float>* breathsOnlyParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    std::unique_ptr<StatsJob> statsJob;
    std::atomic<bool> statsValid { false };
    std::atomic<float> statsFloorDb { -80.0f }, statsSpeechDb { -24.0f };

    juce::TimeSliceThread readAheadThread { "BreathTamer file read-ahead" };
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::File loadedFile;
    std::atomic<bool> fileLoaded { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BreathTamerProcessor)
};
