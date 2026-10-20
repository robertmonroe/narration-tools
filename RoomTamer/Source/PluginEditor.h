#pragma once

#include "PluginProcessor.h"
#include "OfflineRender.h"

namespace ui
{
const juce::Colour body { 0xff12161c }, header { 0xff1d2530 }, footer { 0xff181e26 };
const juce::Colour accent { 0xff9d84f5 }, track { 0xff2a313b }, text { 0xffd6dbe2 }, dim { 0xff8a94a3 };
const juce::Colour pass { 0xff4cc46a }, fail { 0xffe5534b };
}

class RoomTamerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    RoomTamerLookAndFeel();
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float sliderPos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
};

class RenderJob : public juce::Thread
{
public:
    RenderJob (std::vector<std::pair<juce::File, juce::File>> items, rt::Params params);
    ~RenderJob() override;
    void run() override;

    std::atomic<double> progress { 0.0 };
    std::atomic<bool> finished { false };
    juce::String summary; // valid once finished

private:
    std::vector<std::pair<juce::File, juce::File>> items;
    rt::Params params;
    juce::AudioFormatManager formats;
};

class RoomTamerEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit RoomTamerEditor (RoomTamerProcessor&);
    ~RoomTamerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct MeterRow
    {
        juce::Label name, value, verdict;
    };

    void timerCallback() override;
    void setupSlider (juce::Slider&, juce::Label&, const juce::String& title);
    void showRow (MeterRow&, float db, rt::Verdict);
    void chooseFileToOpen();
    void chooseExportTarget();
    void chooseBatchFiles();
    void startJob (std::vector<std::pair<juce::File, juce::File>> items);
    static juce::File processedName (const juce::File& source);

    RoomTamerProcessor& proc;
    RoomTamerLookAndFeel lnf;
    const bool standalone;

    juce::Slider levelSlider, toneSlider;
    juce::Label levelLabel, toneLabel, meterTitle, meterNote, limitsLabel;
    MeterRow floorRow, rmsRow, peakRow;
    juce::ComboBox presetBox;
    juce::TextButton resetBtn { "Reset" }, bypass { "Bypass" };

    std::unique_ptr<SliderAttachment> levelAtt, toneAtt;
    std::unique_ptr<ButtonAttachment> bypassAtt;

    // Standalone-only file bar.
    juce::TextButton openBtn { "Open..." }, playBtn { "Play" }, rewindBtn { "|<" };
    juce::TextButton exportBtn { "Export..." }, batchBtn { "Batch..." };
    juce::Label fileLabel, statusLabel;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<RenderJob> job;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RoomTamerEditor)
};
