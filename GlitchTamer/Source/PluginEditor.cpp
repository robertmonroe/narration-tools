#include "PluginEditor.h"

namespace
{
constexpr int kWidth = 460, kPluginHeight = 290, kFileBarHeight = 92;
constexpr const char* kAbout = "Clicks, pops and bursts of static in pauses, where AI voices join "
                               "audio chunks. Real consonants are left alone.";
constexpr const char* kMonitoring = "LISTENING TO REMOVED GLITCHES ONLY. Untick the box below to hear "
                                    "the repaired narration.";

juce::Font font (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

juce::String formatTime (double s)
{
    const int t = (int) s;
    return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}
} // namespace

//==============================================================================
GlitchTamerLookAndFeel::GlitchTamerLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, ui::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff0b0e12));
    setColour (juce::Slider::textBoxOutlineColourId, ui::track);
    setColour (juce::Label::textColourId, ui::text);
    setColour (juce::TextButton::textColourOffId, ui::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::ProgressBar::foregroundColourId, ui::accent);
    setColour (juce::ProgressBar::backgroundColourId, ui::track);
}

void GlitchTamerLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float sliderPos,
                                               float minPos, float maxPos, juce::Slider::SliderStyle style,
                                               juce::Slider& s)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, sliderPos, minPos, maxPos, style, s);
        return;
    }

    const float cx = (float) x + (float) w * 0.5f, tw = 6.0f;
    const juce::Rectangle<float> rail (cx - tw * 0.5f, (float) y, tw, (float) h);
    g.setColour (ui::track);
    g.fillRoundedRectangle (rail, 3.0f);
    g.setColour (s.isEnabled() ? ui::accent : ui::dim);
    g.fillRoundedRectangle (rail.withTop (sliderPos), 3.0f);

    const juce::Rectangle<float> thumb (cx - 7.0f, sliderPos - 7.0f, 14.0f, 14.0f);
    g.setColour (juce::Colours::white);
    g.fillEllipse (thumb);
    g.setColour (ui::accent.darker (0.3f));
    g.drawEllipse (thumb.reduced (1.0f), 2.0f);
}

void GlitchTamerLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat();
    const auto box = r.removeFromLeft (22.0f).withSizeKeepingCentre (14.0f, 14.0f);
    const bool on = b.getToggleState();

    g.setColour (on ? ui::accent : juce::Colour (0xff0b0e12));
    g.fillRoundedRectangle (box, 2.0f);
    g.setColour (on ? ui::accent : ui::dim);
    g.drawRoundedRectangle (box, 2.0f, 1.2f);
    if (on)
    {
        juce::Path tick;
        tick.startNewSubPath (box.getX() + 3.0f, box.getCentreY());
        tick.lineTo (box.getX() + 6.0f, box.getBottom() - 3.5f);
        tick.lineTo (box.getRight() - 3.0f, box.getY() + 3.5f);
        g.setColour (juce::Colours::black);
        g.strokePath (tick, juce::PathStrokeType (1.8f));
    }

    g.setColour (highlighted ? juce::Colours::white : ui::text);
    g.setFont (font (14.0f));
    g.drawText (b.getButtonText(), r, juce::Justification::centredLeft);
}

void GlitchTamerLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                                   bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = r.getHeight() * 0.5f;
    const bool on = b.getToggleState();

    g.setColour (on ? ui::accent.withAlpha (0.9f) : down ? ui::track.brighter (0.2f) : ui::track.withAlpha (highlighted ? 0.9f : 0.6f));
    g.fillRoundedRectangle (r, radius);
    g.setColour (on ? ui::accent.brighter (0.3f) : ui::dim.withAlpha (0.6f));
    g.drawRoundedRectangle (r, radius, 1.0f);
}

//==============================================================================
RenderJob::RenderJob (std::vector<std::pair<juce::File, juce::File>> i, gt::Params p)
    : juce::Thread ("GlitchTamer render"), items (std::move (i)), params (p)
{
    formats.registerBasicFormats();
}

RenderJob::~RenderJob()
{
    stopThread (10000);
}

void RenderJob::run()
{
    juce::StringArray lines;
    int64_t total = 0;
    const double n = (double) items.size();

    for (size_t i = 0; i < items.size() && ! threadShouldExit(); ++i)
    {
        const auto& [in, out] = items[i];
        const auto r = renderFile (in, out, params, formats, [this, i, n] (double f)
        {
            progress = ((double) i + f) / n;
            return ! threadShouldExit();
        });

        if (r.ok)
        {
            total += r.glitches;
            lines.add (out.getFileName() + ": " + juce::String (r.glitches) + " glitches repaired");
        }
        else
        {
            lines.add (in.getFileName() + ": " + r.error);
        }
    }

    summary = items.size() == 1 ? lines[0]
                                : juce::String (items.size()) + " files, " + juce::String (total) + " glitches repaired";
    progress = 1.0;
    finished = true;
}

//==============================================================================
GlitchTamerEditor::GlitchTamerEditor (GlitchTamerProcessor& p)
    : AudioProcessorEditor (&p), proc (p), standalone (p.isStandalone())
{
    setLookAndFeel (&lnf);

    for (auto* s : { &sensSlider, &widthSlider })
        setupSlider (*s);
    sensAtt  = std::make_unique<SliderAttachment> (proc.apvts, "sens", sensSlider);
    widthAtt = std::make_unique<SliderAttachment> (proc.apvts, "width", widthSlider);

    sensLabel.setText ("Sensitivity", juce::dontSendNotification);
    widthLabel.setText ("Repair width [ms]", juce::dontSendNotification);
    for (auto* l : { &sensLabel, &widthLabel })
    {
        l->setJustificationType (juce::Justification::centred);
        l->setFont (font (14.0f));
        addAndMakeVisible (l);
    }

    countLabel.setText ("Glitches repaired", juce::dontSendNotification);
    countLabel.setFont (font (12.0f));
    countLabel.setColour (juce::Label::textColourId, ui::dim);
    countValue.setFont (font (40.0f, true));
    countValue.setColour (juce::Label::textColourId, ui::accent);
    aboutLabel.setText (kAbout, juce::dontSendNotification);
    aboutLabel.setFont (font (11.0f));
    aboutLabel.setColour (juce::Label::textColourId, ui::dim);
    aboutLabel.setJustificationType (juce::Justification::topLeft);
    for (auto* l : { &countLabel, &countValue, &aboutLabel })
        addAndMakeVisible (l);

    addAndMakeVisible (glitchesOnly);
    glitchesAtt = std::make_unique<ButtonAttachment> (proc.apvts, "glitchesOnly", glitchesOnly);

    bypass.setClickingTogglesState (true);
    addAndMakeVisible (bypass);
    bypassAtt = std::make_unique<ButtonAttachment> (proc.apvts, "bypass", bypass);

    if (standalone)
    {
        for (auto* b : { &openBtn, &playBtn, &rewindBtn, &exportBtn, &batchBtn })
            addAndMakeVisible (b);
        openBtn.onClick   = [this] { chooseFileToOpen(); };
        playBtn.onClick   = [this] { proc.setPlaying (! proc.isPlaying()); };
        rewindBtn.onClick = [this] { proc.rewind(); };
        exportBtn.onClick = [this] { chooseExportTarget(); };
        batchBtn.onClick  = [this] { chooseBatchFiles(); };

        fileLabel.setFont (font (13.0f));
        fileLabel.setText ("No file loaded - live input is processed", juce::dontSendNotification);
        statusLabel.setFont (font (12.0f));
        statusLabel.setColour (juce::Label::textColourId, ui::dim);
        addAndMakeVisible (fileLabel);
        addAndMakeVisible (statusLabel);
        addChildComponent (progressBar);
    }

    setSize (kWidth, kPluginHeight + (standalone ? kFileBarHeight : 0));
    timerCallback();
    startTimerHz (15);
}

GlitchTamerEditor::~GlitchTamerEditor()
{
    stopTimer();
    job.reset();
    setLookAndFeel (nullptr);
}

void GlitchTamerEditor::setupSlider (juce::Slider& s)
{
    s.setSliderStyle (juce::Slider::LinearVertical);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 20);
    s.setNumDecimalPlacesToDisplay (1);
    addAndMakeVisible (s);
}

void GlitchTamerEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::body);

    auto head = getLocalBounds().removeFromTop (40);
    g.setColour (ui::header);
    g.fillRect (head);

    // Logo: a waveform with a glitch spike.
    const auto logo = head.removeFromLeft (44).toFloat().withSizeKeepingCentre (24.0f, 20.0f);
    juce::Path wave;
    wave.startNewSubPath (logo.getX(), logo.getCentreY());
    wave.lineTo (logo.getX() + 8.0f, logo.getCentreY());
    wave.lineTo (logo.getX() + 11.0f, logo.getY());
    wave.lineTo (logo.getX() + 14.0f, logo.getBottom());
    wave.lineTo (logo.getX() + 16.0f, logo.getCentreY());
    wave.lineTo (logo.getRight(), logo.getCentreY());
    g.setColour (ui::accent);
    g.strokePath (wave, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (juce::Colours::white);
    g.setFont (font (19.0f, true));
    g.drawText ("GlitchTamer", head.removeFromLeft (120), juce::Justification::centredLeft);
    g.setColour (ui::text);
    g.setFont (font (15.0f));
    g.drawText ("Seam Repair", head, juce::Justification::centredLeft);

    auto foot = getLocalBounds().withTop (kPluginHeight - 44).withHeight (44);
    g.setColour (ui::footer);
    g.fillRect (foot);
    g.setColour (ui::dim);
    g.setFont (font (11.0f));
    g.drawText ("Latency " + juce::String (proc.getLatencySamples()) + " spls", foot.reduced (14, 0),
                juce::Justification::centredRight);

    if (standalone)
    {
        g.setColour (ui::header);
        g.fillRect (getLocalBounds().withTop (kPluginHeight));
        g.setColour (ui::track);
        g.drawHorizontalLine (kPluginHeight, 0.0f, (float) getWidth());
    }
}

void GlitchTamerEditor::resized()
{
    countLabel.setBounds (18, 56, 130, 18);
    countValue.setBounds (16, 74, 130, 48);
    aboutLabel.setBounds (18, 132, 140, 80);

    sensLabel.setBounds (150, 48, 110, 20);
    sensSlider.setBounds (180, 72, 50, 152);
    widthLabel.setBounds (290, 48, 150, 20);
    widthSlider.setBounds (340, 72, 50, 152);

    glitchesOnly.setBounds (18, 222, 240, 22);
    bypass.setBounds (12, kPluginHeight - 35, 84, 26);

    if (standalone)
    {
        auto bar = getLocalBounds().withTop (kPluginHeight).reduced (12, 10);
        auto row1 = bar.removeFromTop (30);
        openBtn.setBounds (row1.removeFromLeft (76));
        row1.removeFromLeft (6);
        playBtn.setBounds (row1.removeFromLeft (64));
        row1.removeFromLeft (6);
        rewindBtn.setBounds (row1.removeFromLeft (36));
        row1.removeFromLeft (8);
        fileLabel.setBounds (row1);

        bar.removeFromTop (12);
        auto row2 = bar.removeFromTop (30);
        exportBtn.setBounds (row2.removeFromLeft (76));
        row2.removeFromLeft (6);
        batchBtn.setBounds (row2.removeFromLeft (76));
        row2.removeFromLeft (10);
        progressBar.setBounds (row2.withSizeKeepingCentre (row2.getWidth(), 18));
        statusLabel.setBounds (row2);
    }
}

void GlitchTamerEditor::timerCallback()
{
    countValue.setText (juce::String (proc.glitchCount()), juce::dontSendNotification);

    const bool monitoring = proc.currentParams().glitchesOnly;
    aboutLabel.setText (monitoring ? kMonitoring : kAbout, juce::dontSendNotification);
    aboutLabel.setColour (juce::Label::textColourId, monitoring ? ui::accent : ui::dim);

    if (! standalone)
        return;

    playBtn.setButtonText (proc.isPlaying() ? "Pause" : "Play");
    playBtn.setEnabled (proc.hasFile());
    rewindBtn.setEnabled (proc.hasFile());
    exportBtn.setEnabled (proc.hasFile() && job == nullptr);
    batchBtn.setEnabled (job == nullptr);
    openBtn.setEnabled (job == nullptr);

    if (proc.hasFile())
        fileLabel.setText (proc.getLoadedFile().getFileName() + "   " + formatTime (proc.positionSeconds()) + " / "
                               + formatTime (proc.lengthSeconds()),
                           juce::dontSendNotification);

    if (job != nullptr)
    {
        progressValue = job->progress.load();
        if (job->finished.load())
        {
            statusLabel.setText (job->summary, juce::dontSendNotification);
            job.reset();
            progressBar.setVisible (false);
            statusLabel.setVisible (true);
        }
    }
}

juce::File GlitchTamerEditor::processedName (const juce::File& source)
{
    return source.getSiblingFile (source.getFileNameWithoutExtension() + " - GlitchTamer.wav").getNonexistentSibling();
}

void GlitchTamerEditor::chooseFileToOpen()
{
    chooser = std::make_unique<juce::FileChooser> ("Open audio", juce::File(), proc.formats.getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile() && ! proc.loadFile (f))
                                  statusLabel.setText ("Can't open " + f.getFileName(), juce::dontSendNotification);
                          });
}

void GlitchTamerEditor::chooseExportTarget()
{
    const auto source = proc.getLoadedFile();
    chooser = std::make_unique<juce::FileChooser> ("Export repaired audio", processedName (source), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, source] (const juce::FileChooser& fc)
                          {
                              auto out = fc.getResult();
                              if (out == juce::File())
                                  return;
                              startJob ({ { source, out.withFileExtension ("wav") } });
                          });
}

void GlitchTamerEditor::chooseBatchFiles()
{
    chooser = std::make_unique<juce::FileChooser> ("Batch repair (writes '<name> - GlitchTamer.wav' next to each file)",
                                                   juce::File(), proc.formats.getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
                          {
                              std::vector<std::pair<juce::File, juce::File>> items;
                              for (const auto& f : fc.getResults())
                                  items.emplace_back (f, processedName (f));
                              if (! items.empty())
                                  startJob (std::move (items));
                          });
}

void GlitchTamerEditor::startJob (std::vector<std::pair<juce::File, juce::File>> items)
{
    auto params = proc.currentParams();
    params.bypass = false;
    params.glitchesOnly = false;
    progressValue = 0.0;
    statusLabel.setVisible (false);
    progressBar.setVisible (true);
    job = std::make_unique<RenderJob> (std::move (items), params);
    job->startThread();
}
