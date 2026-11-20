#include "PluginEditor.h"

namespace
{
const juce::Colour bg       { 0xff14161c };
const juce::Colour panel    { 0xff1d2029 };
const juce::Colour accent   { 0xff3ec9b6 };
const juce::Colour chordCol { 0xffff9f43 };
const juce::Colour textCol  { 0xffe8eaf0 };
}

VoxChordEditor::VoxChordEditor (VoxChordProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      melodyPad (p, 1, "Drag melody MIDI into FL", accent),
      chordPad (p, 2, "Drag chords MIDI into FL", chordCol)
{
    setSize (700, 560);

    for (auto* b : { &recordBtn, &playBtn, &loadBtn, &rebuildBtn, &exportBtn, &hostBpmBtn })
        addAndMakeVisible (*b);
    addAndMakeVisible (melodyPad);
    addAndMakeVisible (chordPad);

    recordBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffb3333a));

    auto setupLabel = [this] (juce::Label& l, const juce::String& t, juce::Component& target)
    {
        l.setText (t, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, textCol);
        l.attachToComponent (&target, true);
        addAndMakeVisible (l);
    };

    bpmSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    bpmSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
    transposeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    transposeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
    addAndMakeVisible (bpmSlider);
    addAndMakeVisible (transposeSlider);

    gridBox.addItemList ({ "1/32", "1/16", "1/8", "1/4", "Off" }, 1);
    chordLenBox.addItemList ({ "2 beats", "1 bar", "2 bars" }, 1);
    addAndMakeVisible (gridBox);
    addAndMakeVisible (chordLenBox);

    for (auto* t : { &melodyToggle, &chordToggle, &previewToggle, &snapToggle })
    {
        t->setColour (juce::ToggleButton::textColourId, textCol);
        addAndMakeVisible (*t);
    }

    setupLabel (bpmLabel, "BPM", bpmSlider);
    setupLabel (transposeLabel, "Transpose", transposeSlider);
    setupLabel (gridLabel, "Quantise", gridBox);
    setupLabel (chordLenLabel, "Chord length", chordLenBox);

    bpmAtt       = std::make_unique<SliderAtt> (proc.apvts, "bpm", bpmSlider);
    transposeAtt = std::make_unique<SliderAtt> (proc.apvts, "transpose", transposeSlider);
    gridAtt      = std::make_unique<ComboAtt>  (proc.apvts, "grid", gridBox);
    chordLenAtt  = std::make_unique<ComboAtt>  (proc.apvts, "chordLen", chordLenBox);
    melodyAtt    = std::make_unique<ButtonAtt> (proc.apvts, "melody", melodyToggle);
    chordAtt     = std::make_unique<ButtonAtt> (proc.apvts, "chords", chordToggle);
    previewAtt   = std::make_unique<ButtonAtt> (proc.apvts, "preview", previewToggle);
    snapAtt      = std::make_unique<ButtonAtt> (proc.apvts, "snap", snapToggle);
    snapToggle.onClick = [this] { proc.rebuild(); };

    // changing quantise / chord length rebuilds right away
    gridBox.onChange     = [this] { proc.rebuild(); };
    chordLenBox.onChange = [this] { proc.rebuild(); };

    recordBtn.onClick = [this]
    {
        if (proc.isRecording())
            proc.stopRecording();
        else
        {
            const auto err = proc.startRecording();
            if (err.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "VoxChord", err);
        }
    };

    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a recording of your melody",
                                                       juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
                                                       "*.wav;*.mp3;*.aif;*.aiff;*.flac;*.ogg");
        juce::Component::SafePointer<VoxChordEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  const auto f = fc.getResult();
                                  if (safe != nullptr && f != juce::File())
                                      safe->proc.loadAudioFile (f);
                              });
    };

    rebuildBtn.onClick = [this] { proc.rebuild(); };
    playBtn.onClick = [this] { proc.toggleAudition(); };

    hostBpmBtn.onClick = [this]
    {
        const double b = proc.getHostBpm();
        if (b >= 40.0 && b <= 240.0)
        {
            bpmSlider.setValue (b, juce::sendNotificationSync);
            proc.rebuild();
        }
    };

    exportBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Save MIDI file",
                                                       juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                                                           .getChildFile ("VoxChord.mid"),
                                                       "*.mid");
        juce::Component::SafePointer<VoxChordEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (safe != nullptr && f != juce::File())
                                      safe->proc.exportMidi (f.withFileExtension ("mid"));
                              });
    };

    snap = proc.getSnapshot();
    startTimerHz (15);
}

VoxChordEditor::~VoxChordEditor()
{
    stopTimer();
}

void VoxChordEditor::timerCallback()
{
    snap = proc.getSnapshot();
    recordingNow = proc.isRecording();
    level = recordingNow ? proc.getInputLevel() : 0.0f;
    recSeconds = proc.getRecordSeconds();
    recordBtn.setButtonText (recordingNow ? "Stop" : "Record");
    playBtn.setButtonText (proc.isAuditioning() ? "Stop play" : "Play");
    repaint();
}

void VoxChordEditor::resized()
{
    auto r = getLocalBounds().reduced (14);
    r.removeFromTop (34); // title

    auto row = r.removeFromTop (30);
    const int w = (row.getWidth() - 4 * 8) / 5;
    for (auto* b : { &recordBtn, &playBtn, &loadBtn, &rebuildBtn, &exportBtn })
    {
        b->setBounds (row.removeFromLeft (w));
        row.removeFromLeft (8);
    }

    r.removeFromTop (12);
    auto row1 = r.removeFromTop (26);
    bpmSlider.setBounds (row1.removeFromLeft (290).withTrimmedLeft (90));
    row1.removeFromLeft (10);
    hostBpmBtn.setBounds (row1.removeFromLeft (110));
    row1.removeFromLeft (10);
    gridBox.setBounds (row1.removeFromLeft (190).withTrimmedLeft (70));

    r.removeFromTop (8);
    auto row2 = r.removeFromTop (26);
    transposeSlider.setBounds (row2.removeFromLeft (290).withTrimmedLeft (90));
    row2.removeFromLeft (130);
    chordLenBox.setBounds (row2.removeFromLeft (190).withTrimmedLeft (90));

    r.removeFromTop (8);
    auto row3 = r.removeFromTop (24);
    melodyToggle.setBounds (row3.removeFromLeft (170));
    chordToggle.setBounds (row3.removeFromLeft (170));
    previewToggle.setBounds (row3.removeFromLeft (140));
    snapToggle.setBounds (row3.removeFromLeft (130));

    r.removeFromTop (10);
    auto pads = r.removeFromTop (38);
    melodyPad.setBounds (pads.removeFromLeft ((pads.getWidth() - 8) / 2));
    pads.removeFromLeft (8);
    chordPad.setBounds (pads);

    r.removeFromTop (10);
    r.removeFromBottom (60); // status area
    rollArea = r;
}

void VoxChordEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    g.setColour (accent);
    g.setFont (22.0f);
    g.drawText ("VoxChord", 14, 8, 200, 28, juce::Justification::left);
    g.setColour (textCol.withAlpha (0.6f));
    g.setFont (13.0f);
    g.drawText ("voice  >  notes  >  chords      (works fully offline)", 150, 12, 400, 22, juce::Justification::left);

    // piano roll
    g.setColour (panel);
    g.fillRoundedRectangle (rollArea.toFloat(), 8.0f);

    if (snap.loopBeats > 0.0 && (! snap.notes.empty() || ! snap.chords.empty()))
    {
        auto area = rollArea.reduced (10).toFloat();
        area.removeFromTop (16); // room for chord names

        int lo = 127, hi = 0;
        for (const auto& n : snap.notes)  { lo = std::min (lo, n.midi); hi = std::max (hi, n.midi); }
        for (const auto& c : snap.chords) for (int m : c.notes) { lo = std::min (lo, m); hi = std::max (hi, m); }
        lo -= 1; hi += 1;
        const float rowH = area.getHeight() / (float) (hi - lo + 1);
        const float beatW = area.getWidth() / (float) snap.loopBeats;

        // bar lines
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        for (double b = 0.0; b <= snap.loopBeats + 1.0e-6; b += 4.0)
            g.drawVerticalLine ((int) (area.getX() + (float) b * beatW), area.getY(), area.getBottom());

        // chords (orange, behind)
        g.setFont (12.0f);
        for (const auto& c : snap.chords)
        {
            const float x = area.getX() + (float) c.startBeat * beatW;
            const float cw = (float) c.durBeat * beatW;
            for (int m : c.notes)
            {
                const float y = area.getBottom() - (float) (m - lo + 1) * rowH;
                g.setColour (chordCol.withAlpha (0.55f));
                g.fillRoundedRectangle (x + 1.0f, y + 1.0f, std::max (2.0f, cw - 2.0f), std::max (2.0f, rowH - 2.0f), 2.0f);
            }
            g.setColour (chordCol);
            g.drawText (c.name, (int) x + 2, (int) rollArea.getY() + 4, (int) std::max (20.0f, cw), 14, juce::Justification::left);
        }

        // melody (teal, in front)
        g.setColour (accent);
        for (const auto& n : snap.notes)
        {
            const float x = area.getX() + (float) n.startBeat * beatW;
            const float y = area.getBottom() - (float) (n.midi - lo + 1) * rowH;
            g.fillRoundedRectangle (x + 1.0f, y + 1.0f, std::max (2.0f, (float) n.durBeat * beatW - 2.0f),
                                    std::max (2.0f, rowH - 2.0f), 2.0f);
        }
    }
    else
    {
        g.setColour (textCol.withAlpha (0.45f));
        g.setFont (15.0f);
        g.drawFittedText ("Press Record and hum or sing a melody,\nor load a recording of it.",
                          rollArea, juce::Justification::centred, 3);
    }

    // microphone level meter while recording
    if (recordingNow)
    {
        auto m = juce::Rectangle<float> ((float) getWidth() - 214.0f, (float) getHeight() - 40.0f, 200.0f, 12.0f);
        g.setColour (panel);
        g.fillRoundedRectangle (m, 4.0f);
        g.setColour (level > 0.9f ? juce::Colours::red : accent);
        g.fillRoundedRectangle (m.withWidth (std::max (3.0f, m.getWidth() * level)), 4.0f);
        g.setColour (textCol.withAlpha (0.7f));
        g.setFont (12.0f);
        g.drawText ("mic level   " + juce::String (recSeconds, 1) + " s", m.translated (-130.0f, 0.0f).withWidth (126.0f),
                    juce::Justification::right);
    }

    // status
    g.setColour (textCol);
    g.setFont (14.0f);
    g.drawFittedText (snap.status, 14, getHeight() - 56, getWidth() - 28, 34, juce::Justification::left, 2);
    g.setColour (textCol.withAlpha (0.55f));
    g.setFont (12.0f);
    g.drawText ("Key: " + juce::String (snap.keyName) + "    Teal = your melody    Orange = suggested chords",
                14, getHeight() - 22, getWidth() - 28, 18, juce::Justification::left);
}
