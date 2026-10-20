#pragma once

#include "PluginProcessor.h"
#include "OfflineRender.h"

namespace ui
{
const juce::Colour body { 0xff12161c }, header { 0xff1d2530 }, footer { 0xff181e26 };
const juce::Colour accent { 0xff2f7bf5 }, track { 0xff2a313b }, text { 0xffd6dbe2 }, dim { 0xff8a94a3 };
}

class BreathTamerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    BreathTamerLookAndFeel();
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float sliderPos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
};

class ReductionMeter : public juce::Component
{
public:
    void setValueDb (float db);
    void paint (juce::Graphics&) override;

private:
    float valueDb = 0.0f;
};

class RenderJob : public juce::Thread
{
public:
    RenderJob (std::vector<std::pair<juce::File, juce::File>> items, bt::Params params);
    ~RenderJob() override;
    void run() override;

    std::atomic<double> progress { 0.0 };
    std::atomic<bool> finished { false };
    juce::String summary; // valid once finished

private:
    std::vector<std::pair<juce::File, juce::File>> items;
    bt::Params params;
    juce::AudioFormatManager formats;
};

class BreathTamerEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BreathTamerEditor (BreathTamerProcessor&);
    ~BreathTamerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void timerCallback() override;
    void setChoice (const juce::String& paramId, int index);
    void setupSlider (juce::Slider&);
    void setupRadio (juce::ToggleButton&, int group);
    void chooseFileToOpen();
    void chooseExportTarget();
    void chooseBatchFiles();
    void startJob (std::vector<std::pair<juce::File, juce::File>> items);
    static juce::File processedName (const juce::File& source);

    BreathTamerProcessor& proc;
    BreathTamerLookAndFeel lnf;
    const bool standalone;

    juce::ComboBox algorithm;
    juce::Label algorithmLabel, algorithmHint;
    juce::ToggleButton targetMode { "Target" }, gainMode { "Gain" };
    juce::ToggleButton naturalMode { "Natural" }, gatedMode { "Gated" };
    juce::Slider levelSlider, sensSlider;
    juce::Label levelLabel, sensLabel;
    ReductionMeter meter;
    juce::ToggleButton breathsOnly { "Output breaths only" };
    juce::ToggleButton strictRoomTone { "Room tone fix" };
    juce::TextButton bypass { "Bypass" };

    std::unique_ptr<SliderAttachment> levelAtt, sensAtt;
    std::unique_ptr<ButtonAttachment> breathsAtt, strictAtt, bypassAtt;
    std::unique_ptr<ComboAttachment> algorithmAtt;

    // Standalone-only file bar.
    juce::TextButton openBtn { "Open..." }, playBtn { "Play" }, rewindBtn { "|<" };
    juce::TextButton exportBtn { "Export..." }, batchBtn { "Batch..." };
    juce::Label fileLabel, statusLabel;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<RenderJob> job;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BreathTamerEditor)
};
