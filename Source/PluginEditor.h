#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

//==============================================================================
// A pad you grab with the mouse and drop into FL Studio (piano roll, playlist, channel rack)
class MidiDragPad : public juce::Component
{
public:
    MidiDragPad (VoxChordProcessor& p, int partToExport, const juce::String& text, juce::Colour colour)
        : proc (p), part (partToExport), label (text), col (colour)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (col.withAlpha (0.18f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (col);
        g.drawRoundedRectangle (r, 8.0f, 1.5f);
        g.setFont (14.0f);
        g.drawText (label, getLocalBounds(), juce::Justification::centred);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || e.getDistanceFromDragStart() < 6)
            return;

        dragging = true;
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile (part == 1 ? "VoxChord_melody.mid"
                                                       : part == 2 ? "VoxChord_chords.mid"
                                                                   : "VoxChord_melody_and_chords.mid");
        if (proc.exportMidi (file, part, false))
            juce::DragAndDropContainer::performExternalDragDropOfFiles (juce::StringArray (file.getFullPathName()), false, this,
                                                                         [this] { dragging = false; });
        else
            dragging = false;
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

private:
    VoxChordProcessor& proc;
    int part;
    juce::String label;
    juce::Colour col;
    bool dragging = false;
};

//==============================================================================
class VoxChordEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit VoxChordEditor (VoxChordProcessor&);
    ~VoxChordEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    VoxChordProcessor& proc;
    VoxChordProcessor::Snapshot snap;
    float level = 0.0f;
    double recSeconds = 0.0;
    bool recordingNow = false;

    juce::TextButton recordBtn { "Record" }, playBtn { "Play" }, loadBtn { "Load audio file" },
                     rebuildBtn { "Rebuild" }, exportBtn { "Export MIDI" }, hostBpmBtn { "Use host BPM" };

    juce::Slider bpmSlider, transposeSlider;
    juce::ComboBox gridBox, chordLenBox;
    juce::ToggleButton melodyToggle { "Melody MIDI (ch 1)" },
                       chordToggle { "Chord MIDI (ch 2)" },
                       previewToggle { "Preview sound" },
                       snapToggle { "Snap to key" };
    juce::Label bpmLabel, transposeLabel, gridLabel, chordLenLabel;

    MidiDragPad melodyPad, chordPad;

    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SliderAtt> bpmAtt, transposeAtt;
    std::unique_ptr<ComboAtt> gridAtt, chordLenAtt;
    std::unique_ptr<ButtonAtt> melodyAtt, chordAtt, previewAtt, snapAtt;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> rollArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxChordEditor)
};
