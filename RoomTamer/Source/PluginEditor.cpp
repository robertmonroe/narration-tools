#include "PluginEditor.h"

namespace
{
constexpr int kWidth = 460, kPluginHeight = 290, kFileBarHeight = 92;

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
RoomTamerLookAndFeel::RoomTamerLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, ui::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff0b0e12));
    setColour (juce::Slider::textBoxOutlineColourId, ui::track);
    setColour (juce::Label::textColourId, ui::text);
    setColour (juce::TextButton::textColourOffId, ui::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::ProgressBar::foregroundColourId, ui::accent);
    setColour (juce::ProgressBar::backgroundColourId, ui::track);
    setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff0b0e12));
    setColour (juce::ComboBox::outlineColourId, ui::track);
    setColour (juce::ComboBox::textColourId, ui::text);
    setColour (juce::ComboBox::arrowColourId, ui::accent);
    setColour (juce::PopupMenu::backgroundColourId, ui::header);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, ui::accent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
}

void RoomTamerLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float sliderPos,
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

void RoomTamerLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
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
RenderJob::RenderJob (std::vector<std::pair<juce::File, juce::File>> i, rt::Params p)
    : juce::Thread ("RoomTamer render"), items (std::move (i)), params (p)
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
    int passed = 0, failed = 0;
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
            (allPass (r.reading) ? passed : failed) += 1;
            lines.add (out.getFileName() + ": " + describe (r.reading));
        }
        else
        {
            ++failed;
            lines.add (in.getFileName() + ": " + r.error);
        }
    }

    if (items.size() == 1)
        summary = lines[0];
    else
        summary = juce::String (items.size()) + " files: " + juce::String (passed) + " pass ACX"
                + (failed > 0 ? ", " + juce::String (failed) + " need attention (export one to see why)" : juce::String());
    progress = 1.0;
    finished = true;
}

//==============================================================================
RoomTamerEditor::RoomTamerEditor (RoomTamerProcessor& p)
    : AudioProcessorEditor (&p), proc (p), standalone (p.isStandalone())
{
    setLookAndFeel (&lnf);

    setupSlider (levelSlider, levelLabel, "Level [dB]");
    setupSlider (toneSlider, toneLabel, "Tone");
    levelAtt = std::make_unique<SliderAttachment> (proc.apvts, "level", levelSlider);
    toneAtt  = std::make_unique<SliderAttachment> (proc.apvts, "tone", toneSlider);
    toneSlider.setNumDecimalPlacesToDisplay (0);
    levelSlider.setTooltip ("Room tone level, measured the way ACX measures the noise floor. ACX allows -60 dB or lower.");
    toneSlider.setTooltip ("Darker (negative) or brighter (positive) room.");

    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem (proc.getProgramName (i), i + 1);
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { proc.applyProgram (presetBox.getSelectedId() - 1); };
    presetBox.setTooltip ("Presets. Choose one to set Level and Tone.");
    addAndMakeVisible (presetBox);

    meterTitle.setText ("ACX check", juce::dontSendNotification);
    meterTitle.setFont (font (14.0f, true));
    meterNote.setText ("since playback started", juce::dontSendNotification);
    meterNote.setFont (font (11.0f));
    meterNote.setColour (juce::Label::textColourId, ui::dim);
    limitsLabel.setText ("ACX limits (dB): floor -60 or lower, never dead silence; RMS -23 to -18; peak -3 or lower.",
                         juce::dontSendNotification);
    limitsLabel.setFont (font (11.0f));
    limitsLabel.setColour (juce::Label::textColourId, ui::dim);
    limitsLabel.setJustificationType (juce::Justification::topLeft);
    for (auto* l : { &meterTitle, &meterNote, &limitsLabel })
        addAndMakeVisible (l);

    const std::pair<MeterRow*, const char*> rows[] = { { &floorRow, "Noise floor" }, { &rmsRow, "RMS" }, { &peakRow, "Peak" } };
    for (auto [row, name] : rows)
    {
        row->name.setText (name, juce::dontSendNotification);
        row->name.setFont (font (13.0f));
        row->value.setFont (font (15.0f, true));
        row->value.setJustificationType (juce::Justification::centredRight);
        row->verdict.setFont (font (12.0f, true));
        row->verdict.setJustificationType (juce::Justification::centred);
        for (auto* l : { &row->name, &row->value, &row->verdict })
            addAndMakeVisible (l);
    }

    resetBtn.onClick = [this] { proc.resetMeter(); };
    resetBtn.setTooltip ("Start a new ACX reading.");
    addAndMakeVisible (resetBtn);

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
    startTimerHz (10);
}

RoomTamerEditor::~RoomTamerEditor()
{
    stopTimer();
    job.reset();
    setLookAndFeel (nullptr);
}

void RoomTamerEditor::setupSlider (juce::Slider& s, juce::Label& l, const juce::String& title)
{
    s.setSliderStyle (juce::Slider::LinearVertical);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 20);
    s.setNumDecimalPlacesToDisplay (1);
    addAndMakeVisible (s);
    l.setText (title, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setFont (font (13.0f));
    addAndMakeVisible (l);
}

void RoomTamerEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::body);

    auto head = getLocalBounds().removeFromTop (40);
    g.setColour (ui::header);
    g.fillRect (head);

    // Logo: a room corner with a faint floor of noise.
    const auto logo = head.removeFromLeft (44).toFloat().withSizeKeepingCentre (24.0f, 18.0f);
    juce::Path room;
    room.startNewSubPath (logo.getX(), logo.getY());
    room.lineTo (logo.getX(), logo.getBottom());
    room.lineTo (logo.getRight(), logo.getBottom());
    g.setColour (ui::accent);
    g.strokePath (room, juce::PathStrokeType (2.0f));
    juce::Random rnd (7);
    for (int i = 0; i < 9; ++i)
        g.fillRect (logo.getX() + 3.0f + (float) i * 2.3f, logo.getBottom() - 4.0f - rnd.nextFloat() * 3.0f, 1.4f, 1.4f);

    g.setColour (juce::Colours::white);
    g.setFont (font (19.0f, true));
    g.drawText ("RoomTamer", head.removeFromLeft (112), juce::Justification::centredLeft);
    g.setColour (ui::text);
    g.setFont (font (15.0f));
    g.drawText ("Room Tone for ACX", head, juce::Justification::centredLeft);

    auto foot = getLocalBounds().withTop (kPluginHeight - 44).withHeight (44);
    g.setColour (ui::footer);
    g.fillRect (foot);
    g.setColour (ui::dim);
    g.setFont (font (11.0f));
    g.drawText ("Put last on the master, after loudness and limiter", foot.reduced (14, 0),
                juce::Justification::centredRight);

    if (standalone)
    {
        g.setColour (ui::header);
        g.fillRect (getLocalBounds().withTop (kPluginHeight));
        g.setColour (ui::track);
        g.drawHorizontalLine (kPluginHeight, 0.0f, (float) getWidth());
    }
}

void RoomTamerEditor::resized()
{
    meterTitle.setBounds (14, 50, 90, 22);
    meterNote.setBounds (96, 52, 140, 20);
    int y = 80;
    for (auto* row : { &floorRow, &rmsRow, &peakRow })
    {
        row->name.setBounds (14, y, 92, 26);
        row->value.setBounds (100, y, 82, 26);
        row->verdict.setBounds (188, y + 3, 50, 20);
        y += 30;
    }
    limitsLabel.setBounds (14, 172, 232, 36);
    resetBtn.setBounds (18, 210, 70, 24);

    presetBox.setBounds (262, 50, 184, 24);
    levelLabel.setBounds (258, 82, 92, 20);
    levelSlider.setBounds (280, 104, 50, 130);
    toneLabel.setBounds (358, 82, 92, 20);
    toneSlider.setBounds (380, 104, 50, 130);

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

void RoomTamerEditor::showRow (MeterRow& row, float db, rt::Verdict v)
{
    row.value.setText (v == rt::Verdict::none ? juce::String ("--")
                       : db <= -199.0f        ? juce::String ("silence")
                                              : juce::String (db, 1) + " dB",
                       juce::dontSendNotification);
    row.verdict.setText (v == rt::Verdict::pass ? "PASS" : v == rt::Verdict::fail ? "FAIL" : "", juce::dontSendNotification);
    row.verdict.setColour (juce::Label::textColourId, v == rt::Verdict::pass ? ui::pass : ui::fail);
}

void RoomTamerEditor::timerCallback()
{
    const auto r = proc.reading();
    showRow (floorRow, r.floorDb, rt::floorVerdict (r));
    showRow (rmsRow, r.rmsDb, rt::rmsVerdict (r));
    showRow (peakRow, r.peakDb, rt::peakVerdict (r));
    meterNote.setText (r.seconds > 0.0 ? "over " + formatTime (r.seconds) + " played" : juce::String ("press play to measure"),
                       juce::dontSendNotification);
    if (presetBox.getSelectedId() != proc.getCurrentProgram() + 1)
        presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);

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
            statusLabel.setTooltip (job->summary);
            job.reset();
            progressBar.setVisible (false);
            statusLabel.setVisible (true);
        }
    }
}

juce::File RoomTamerEditor::processedName (const juce::File& source)
{
    return source.getSiblingFile (source.getFileNameWithoutExtension() + " - RoomTamer.wav").getNonexistentSibling();
}

void RoomTamerEditor::chooseFileToOpen()
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

void RoomTamerEditor::chooseExportTarget()
{
    const auto source = proc.getLoadedFile();
    chooser = std::make_unique<juce::FileChooser> ("Export with room tone", processedName (source), "*.wav");
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

void RoomTamerEditor::chooseBatchFiles()
{
    chooser = std::make_unique<juce::FileChooser> ("Batch (writes '<name> - RoomTamer.wav' next to each file)",
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

void RoomTamerEditor::startJob (std::vector<std::pair<juce::File, juce::File>> items)
{
    auto params = proc.currentParams();
    params.bypass = false;
    progressValue = 0.0;
    statusLabel.setVisible (false);
    progressBar.setVisible (true);
    job = std::make_unique<RenderJob> (std::move (items), params);
    job->startThread();
}
