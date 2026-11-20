#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <vector>

#include "Analysis.h"

//==============================================================================
// Small built-in sound so you can hear the result without any routing.
struct PreviewSound : public juce::SynthesiserSound
{
    bool appliesToNote (int) override    { return true; }
    bool appliesToChannel (int) override { return true; }
};

struct PreviewVoice : public juce::SynthesiserVoice
{
    bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<PreviewSound*> (s) != nullptr; }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        level = velocity * 0.18f;
        freq = juce::MidiMessage::getMidiNoteInHertz (midiNote);
        phase = 0.0;
        adsr.setSampleRate (getSampleRate());
        adsr.setParameters (juce::ADSR::Parameters (0.01f, 0.10f, 0.70f, 0.20f));
        adsr.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
            adsr.noteOff();
        else
        {
            adsr.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) override
    {
        if (! adsr.isActive())
            return;

        const double inc = juce::MathConstants<double>::twoPi * freq / getSampleRate();
        for (int i = 0; i < numSamples; ++i)
        {
            const float env = adsr.getNextSample();
            const float s = (float) (std::sin (phase) + 0.3 * std::sin (2.0 * phase)) * level * env;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.addSample (ch, startSample + i, s);
            phase += inc;

            if (! adsr.isActive())
            {
                clearCurrentNote();
                break;
            }
        }
    }

private:
    juce::ADSR adsr;
    double phase = 0.0, freq = 440.0;
    float level = 0.2f;
};

//==============================================================================
class VoxChordProcessor : public juce::AudioProcessor,
                          private juce::AudioIODeviceCallback
{
public:
    VoxChordProcessor();
    ~VoxChordProcessor() override;

    // --- AudioProcessor ---
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "VoxChord"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- features used by the editor ---
    juce::String startRecording();       // returns an error message, or empty if OK
    void stopRecording();                // stops and analyses what was recorded
    bool isRecording() const { return recording.load(); }
    void loadAudioFile (const juce::File& file);
    void rebuild();                      // re-quantise / re-detect key / re-make chords from the raw notes
    bool exportMidi (const juce::File& file);
    double getHostBpm() const { return hostBpm.load(); }

    struct Snapshot
    {
        std::vector<vc::Note> notes;
        std::vector<vc::Chord> chords;
        std::string keyName = "-";
        double loopBeats = 0.0;
        juce::String status;
    };
    Snapshot getSnapshot();

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void setStatus (const juce::String& s);
    void analyseAsync (std::shared_ptr<std::vector<float>> mono, double sr);

    // AudioIODeviceCallback (microphone capture)
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    void releaseAllNotes (juce::MidiBuffer& midi, int samplePos);

    // analysis data (protected by dataLock)
    juce::CriticalSection dataLock;
    std::vector<vc::RawNote> rawNotes;
    std::vector<vc::Note> notes;
    std::vector<vc::Chord> chords;
    vc::KeyInfo key;
    double loopBeats = 0.0;
    juce::String status { "Ready. Record your voice or load an audio file." };

    // recording
    std::unique_ptr<juce::AudioDeviceManager> deviceManager;
    std::vector<float> recBuffer;
    std::atomic<int> writePos { 0 };
    double recSampleRate = 44100.0;
    std::atomic<bool> recording { false };

    juce::AudioFormatManager formats;
    juce::ThreadPool pool { 1 };

    // playback (audio thread only)
    bool sounding[2][128] = {};
    double lastPpqEnd = -1.0;
    int lastTranspose = 0;
    bool lastMel = true, lastChd = true, lastPreview = true, lockMissed = false;
    double currentSampleRate = 44100.0;
    std::atomic<double> hostBpm { 0.0 };

    juce::Synthesiser synth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxChordProcessor)
};
