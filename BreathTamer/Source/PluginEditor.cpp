#include "PluginEditor.h"

namespace
{
constexpr int kWidth = 460, kPluginHeight = 290, kFileBarHeight = 92;
constexpr float kMeterRangeDb = 60.0f;

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
BreathTamerLookAndFeel::BreathTamerLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, ui::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff0b0e12));
    setColour (juce::Slider::textBoxOutlineColourId, ui::track);
    setColour (juce::Label::textColourId, ui::text);
    setColour (juce::TextButton::textColourOffId, ui::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::ProgressBar::foregroundColourId, ui::accent);
    setColour (juce::ProgressBar::backgroundColourId, ui::track);
    setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff0b0e12));
    setColour (juce::ComboBox::outlineColourId, ui::track);
    setColour (juce::ComboBox::textColourId, ui::text);
    setColour (juce::ComboBox::arrowColourId, ui::dim);
    setColour (juce::PopupMenu::backgroundColourId, ui::header);
    setColour (juce::PopupMenu::textColourId, ui::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, ui::accent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
}

void BreathTamerLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float sliderPos,
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

void BreathTamerLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat();
    const auto box = r.removeFromLeft (22.0f).withSizeKeepingCentre (14.0f, 14.0f);
    const bool on = b.getToggleState();
    g.setOpacity (b.isEnabled() ? 1.0f : 0.35f);

    if ((bool) b.getProperties()["radio"])
    {
        g.setColour (on ? ui::accent : juce::Colour (0xff5b6573));
        g.fillEllipse (box);
        if (on)
        {
            g.setColour (ui::accent.brighter (0.4f));
            g.drawEllipse (box, 1.0f);
        }
    }
    else
    {
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
            g.setColour (juce::Colours::white);
            g.strokePath (tick, juce::PathStrokeType (1.8f));
        }
    }

    g.setColour (highlighted ? juce::Colours::white : ui::text);
    g.setFont (font (14.0f));
    g.drawText (b.getButtonText(), r, juce::Justification::centredLeft);
}

void BreathTamerLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                                   bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = r.getHeight() * 0.5f;
    const bool on = b.getToggleState();

    g.setColour (on ? ui::accent.withAlpha (0.85f) : down ? ui::track.brighter (0.2f) : ui::track.withAlpha (highlighted ? 0.9f : 0.6f));
    g.fillRoundedRectangle (r, radius);
    g.setColour (on ? ui::accent.brighter (0.3f) : ui::dim.withAlpha (0.6f));
    g.drawRoundedRectangle (r, radius, 1.0f);
}

//==============================================================================
void ReductionMeter::setValueDb (float db)
{
    db = juce::jlimit (-kMeterRangeDb, 0.0f, db);
    // Instant attack, gentle release for readability.
    valueDb = db < valueDb ? db : valueDb + (db - valueDb) * 0.25f;
    repaint();
}

void ReductionMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const auto label = r.removeFromBottom (22.0f);
    const auto bar = r.withSizeKeepingCentre (8.0f, r.getHeight());

    g.setColour (ui::track);
    g.fillRoundedRectangle (bar, 2.0f);
    g.setColour (ui::accent);
    g.fillRoundedRectangle (bar.withHeight (bar.getHeight() * (-valueDb / kMeterRangeDb)), 2.0f);

    g.setColour (ui::text);
    g.setFont (font (13.0f));
    g.drawText (juce::String (std::abs (valueDb) < 0.05f ? 0.0f : valueDb, 1), label, juce::Justification::centred);
}

//==============================================================================
RenderJob::RenderJob (std::vector<std::pair<juce::File, juce::File>> i, bt::Params p)
    : juce::Thread ("BreathTamer render"), items (std::move (i)), params (p)
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
    int64_t totalBreaths = 0;
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
            totalBreaths += r.breaths;
            lines.add (out.getFileName() + ": " + juce::String (r.breaths) + " breaths");
        }
        else
        {
            lines.add (in.getFileName() + ": " + r.error);
        }
    }

    summary = items.size() == 1 ? lines[0]
                                : juce::String (items.size()) + " files, " + juce::String (totalBreaths) + " breaths";
    progress = 1.0;
    finished = true;
}

//==============================================================================
BreathTamerEditor::BreathTamerEditor (BreathTamerProcessor& p)
    : AudioProcessorEditor (&p), proc (p), standalone (p.isStandalone())
{
    setLookAndFeel (&lnf);

    algorithm.addItemList ({ "Real-time", "Offline", "Classic" }, 1);
    addAndMakeVisible (algorithm);
    algorithmAtt = std::make_unique<ComboAttachment> (proc.apvts, "algorithm", algorithm);
    algorithmLabel.setText ("Algorithm", juce::dontSendNotification);
    algorithmLabel.setFont (font (12.0f));
    algorithmLabel.setColour (juce::Label::textColourId, ui::dim);
    algorithmHint.setFont (font (11.0f));
    algorithmHint.setColour (juce::Label::textColourId, ui::dim);
    algorithmHint.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (algorithmLabel);
    addAndMakeVisible (algorithmHint);

    setupRadio (targetMode, 1001);
    setupRadio (gainMode, 1001);
    setupRadio (naturalMode, 1002);
    setupRadio (gatedMode, 1002);
    gainMode.onClick    = [this] { if (gainMode.getToggleState()) setChoice ("mode", 0); };
    targetMode.onClick  = [this] { if (targetMode.getToggleState()) setChoice ("mode", 1); };
    naturalMode.onClick = [this] { if (naturalMode.getToggleState()) setChoice ("ambience", 0); };
    gatedMode.onClick   = [this] { if (gatedMode.getToggleState()) setChoice ("ambience", 1); };

    for (auto* s : { &levelSlider, &sensSlider })
        setupSlider (*s);
    levelAtt = std::make_unique<SliderAttachment> (proc.apvts, "level", levelSlider);
    sensAtt  = std::make_unique<SliderAttachment> (proc.apvts, "sens", sensSlider);

    for (auto* l : { &levelLabel, &sensLabel })
    {
        l->setJustificationType (juce::Justification::centred);
        l->setFont (font (14.0f));
        addAndMakeVisible (l);
    }
    sensLabel.setText ("Sensitivity", juce::dontSendNotification);

    addAndMakeVisible (meter);
    addAndMakeVisible (breathsOnly);
    breathsAtt = std::make_unique<ButtonAttachment> (proc.apvts, "breathsOnly", breathsOnly);
    strictRoomTone.setTooltip ("Natural mode: learn room tone only from audio well below speech. "
                               "Affects the first breath after playback starts mid-speech.");
    addAndMakeVisible (strictRoomTone);
    strictAtt = std::make_unique<ButtonAttachment> (proc.apvts, "strictAmbience", strictRoomTone);

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
    startTimerHz (30);
}

BreathTamerEditor::~BreathTamerEditor()
{
    stopTimer();
    job.reset();
    setLookAndFeel (nullptr);
}

void BreathTamerEditor::setupSlider (juce::Slider& s)
{
    s.setSliderStyle (juce::Slider::LinearVertical);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 20);
    s.setNumDecimalPlacesToDisplay (1);
    addAndMakeVisible (s);
}

void BreathTamerEditor::setupRadio (juce::ToggleButton& b, int group)
{
    b.setRadioGroupId (group);
    b.getProperties().set ("radio", true);
    addAndMakeVisible (b);
}

void BreathTamerEditor::setChoice (const juce::String& paramId, int index)
{
    if (auto* p = proc.apvts.getParameter (paramId))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) index));
        p->endChangeGesture();
    }
}

void BreathTamerEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::body);

    auto r = getLocalBounds();
    auto head = r.removeFromTop (40);
    g.setColour (ui::header);
    g.fillRect (head);

    // Logo: concentric breath rings.
    const auto logo = head.removeFromLeft (44).toFloat().withSizeKeepingCentre (22.0f, 22.0f);
    g.setColour (ui::accent);
    for (int i = 0; i < 3; ++i)
        g.drawEllipse (logo.reduced ((float) i * 4.0f), 1.6f);

    g.setColour (juce::Colours::white);
    g.setFont (font (19.0f, true));
    g.drawText ("BreathTamer", head.removeFromLeft (124), juce::Justification::centredLeft);
    g.setColour (ui::text);
    g.setFont (font (15.0f));
    g.drawText ("Breath Control", head, juce::Justification::centredLeft);

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

void BreathTamerEditor::resized()
{
    algorithmLabel.setBounds (18, 46, 116, 16);
    algorithm.setBounds (20, 64, 116, 24);
    algorithmHint.setBounds (18, 90, 130, 30);
    targetMode.setBounds (22, 124, 100, 22);
    gainMode.setBounds (22, 146, 100, 22);
    naturalMode.setBounds (22, 182, 100, 22);
    gatedMode.setBounds (22, 204, 100, 22);

    levelLabel.setBounds (128, 48, 110, 20);
    levelSlider.setBounds (158, 72, 50, 152);

    meter.setBounds (236, 74, 44, 150);

    sensLabel.setBounds (284, 48, 110, 20);
    sensSlider.setBounds (314, 72, 50, 152);

    breathsOnly.setBounds (282, 222, 170, 22);
    bypass.setBounds (12, kPluginHeight - 35, 84, 26);
    strictRoomTone.setBounds (110, kPluginHeight - 35, 150, 26);

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

void BreathTamerEditor::timerCallback()
{
    const auto params = proc.currentParams();
    const bool target = params.mode == bt::Params::Target;
    gainMode.setToggleState (! target, juce::dontSendNotification);
    targetMode.setToggleState (target, juce::dontSendNotification);
    naturalMode.setToggleState (params.natural, juce::dontSendNotification);
    gatedMode.setToggleState (! params.natural, juce::dontSendNotification);
    levelLabel.setText (target ? "Level [dBFS]" : "Level [dB]", juce::dontSendNotification);

    const bool classic = params.algorithm == bt::Params::Classic;
    naturalMode.setEnabled (! classic);
    gatedMode.setEnabled (! classic);
    strictRoomTone.setEnabled (! classic && params.natural); // only Natural learns room tone

    juce::String hint;
    if (classic)
        hint = "BreathTamer 1.0 detection, gated only";
    else if (params.algorithm == bt::Params::Offline)
        hint = ! standalone ? "Needs the whole file; plugin runs as Real-time"
             : proc.offlineStatsReady() ? "Whole-file analysis ready"
             : proc.isAnalysing() ? "Analysing file..."
             : "Export/Batch analyse the whole file";
    algorithmHint.setText (hint, juce::dontSendNotification);

    meter.setValueDb (params.bypass || params.breathsOnly ? 0.0f : proc.meterDb());

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

juce::File BreathTamerEditor::processedName (const juce::File& source)
{
    return source.getSiblingFile (source.getFileNameWithoutExtension() + " - BreathTamer.wav").getNonexistentSibling();
}

void BreathTamerEditor::chooseFileToOpen()
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

void BreathTamerEditor::chooseExportTarget()
{
    const auto source = proc.getLoadedFile();
    chooser = std::make_unique<juce::FileChooser> ("Export processed audio", processedName (source), "*.wav");
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

void BreathTamerEditor::chooseBatchFiles()
{
    chooser = std::make_unique<juce::FileChooser> ("Batch process (writes '<name> - BreathTamer.wav' next to each file)",
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

void BreathTamerEditor::startJob (std::vector<std::pair<juce::File, juce::File>> items)
{
    auto params = proc.currentParams();
    params.bypass = false;
    progressValue = 0.0;
    statusLabel.setVisible (false);
    progressBar.setVisible (true);
    job = std::make_unique<RenderJob> (std::move (items), params);
    job->startThread();
}
