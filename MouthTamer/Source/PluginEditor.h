#pragma once

#include "PluginProcessor.h"
#include "OfflineRender.h"

namespace ui
{
const juce::Colour body { 0xff12161c }, header { 0xff1d2530 }, footer { 0xff181e26 };
const juce::Colour accent { 0xff35c2b0 }, track { 0xff2a313b }, text { 0xffd6dbe2 }, dim { 0xff8a94a3 };
}

class MouthTamerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MouthTamerLookAndFeel();
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float sliderPos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
};

class RenderJob : public juce::Thread
{
public:
    RenderJob (std::vector<std::pair<juce::File, juce::File>> items, mt::Params params);
    ~RenderJob() override;
    void run() override;

    std::atomic<double> progress { 0.0 };
    std::atomic<bool> finished { false };
    juce::String summary; // valid once finished

private:
    std::vector<std::pair<juce::File, juce::File>> items;
    mt::Params params;
    juce::AudioFormatManager formats;
};

class MouthTamerEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MouthTamerEditor (MouthTamerProcessor&);
    ~MouthTamerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void timerCallback() override;
    void setupSlider (juce::Slider&, juce::Label&, const juce::String& title);
    void chooseFileToOpen();
    void chooseExportTarget();
    void chooseBatchFiles();
    void startJob (std::vector<std::pair<juce::File, juce::File>> items);
    static juce::File processedName (const juce::File& source);

    MouthTamerProcessor& proc;
    MouthTamerLookAndFeel lnf;
    const bool standalone;

    juce::Slider sensSlider, skewSlider, wideningSlider;
    juce::Label sensLabel, skewLabel, wideningLabel, countLabel, countValue, aboutLabel;
    juce::ToggleButton clicksOnly { "Hear only the removed clicks" };
    juce::TextButton bypass { "Bypass" };

    std::unique_ptr<SliderAttachment> sensAtt, skewAtt, wideningAtt;
    std::unique_ptr<ButtonAttachment> clicksAtt, bypassAtt;

    // Standalone-only file bar.
    juce::TextButton openBtn { "Open..." }, playBtn { "Play" }, rewindBtn { "|<" };
    juce::TextButton exportBtn { "Export..." }, batchBtn { "Batch..." };
    juce::Label fileLabel, statusLabel;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<RenderJob> job;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MouthTamerEditor)
};
