#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

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

    juce::TextButton recordBtn { "Record" }, loadBtn { "Load audio file" }, rebuildBtn { "Rebuild" },
                     exportBtn { "Export MIDI" }, hostBpmBtn { "Use host BPM" };

    juce::Slider bpmSlider, transposeSlider;
    juce::ComboBox gridBox, chordLenBox;
    juce::ToggleButton melodyToggle { "Melody MIDI (channel 1)" },
                       chordToggle { "Chord MIDI (channel 2)" },
                       previewToggle { "Preview sound" };
    juce::Label bpmLabel, transposeLabel, gridLabel, chordLenLabel;

    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SliderAtt> bpmAtt, transposeAtt;
    std::unique_ptr<ComboAtt> gridAtt, chordLenAtt;
    std::unique_ptr<ButtonAtt> melodyAtt, chordAtt, previewAtt;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> rollArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxChordEditor)
};
