#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout VoxChordProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "bpm", 1 }, "BPM",
                                                        NormalisableRange<float> (40.0f, 240.0f, 0.1f), 120.0f));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "grid", 1 }, "Quantise",
                                                         StringArray { "1/32", "1/16", "1/8", "1/4", "Off" }, 1));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "chordLen", 1 }, "Chord length",
                                                         StringArray { "2 beats", "1 bar", "2 bars" }, 1));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "melody", 1 }, "Melody MIDI out", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "chords", 1 }, "Chord MIDI out", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "preview", 1 }, "Preview sound", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "snap", 1 }, "Snap to key", true));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "transpose", 1 }, "Melody transpose", -24, 24, 0));

    return { p.begin(), p.end() };
}

//==============================================================================
VoxChordProcessor::VoxChordProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    formats.registerBasicFormats();

    synth.addSound (new PreviewSound());
    for (int i = 0; i < 16; ++i)
        synth.addVoice (new PreviewVoice());
}

VoxChordProcessor::~VoxChordProcessor()
{
    if (recording.load() && deviceManager != nullptr)
    {
        deviceManager->removeAudioCallback (this);
        deviceManager->closeAudioDevice();
    }
    pool.removeAllJobs (true, 5000);
}

void VoxChordProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    synth.setCurrentPlaybackSampleRate (sampleRate);
    lastPpqEnd = -1.0;
}

bool VoxChordProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

juce::AudioProcessorEditor* VoxChordProcessor::createEditor()
{
    return new VoxChordEditor (*this);
}

//==============================================================================
void VoxChordProcessor::setStatus (const juce::String& s)
{
    const juce::ScopedLock sl (dataLock);
    status = s;
}

float VoxChordProcessor::getInputLevel()
{
    const float p = inputPeak.load();
    inputPeak.store (p * 0.8f);
    return juce::jlimit (0.0f, 1.0f, std::sqrt (p) * 1.4f);
}

void VoxChordProcessor::toggleAudition()
{
    if (auditioning.load())
        auditioning.store (false);
    else
    {
        auditionRestart.store (true);
        auditioning.store (true);
    }
}

VoxChordProcessor::Snapshot VoxChordProcessor::getSnapshot()
{
    const juce::ScopedLock sl (dataLock);
    Snapshot s;
    s.notes = notes;
    s.chords = chords;
    s.keyName = key.name;
    s.loopBeats = loopBeats;
    s.status = status;
    return s;
}

//==============================================================================
// Recording from the microphone (works offline, uses the system default input)
//==============================================================================
juce::String VoxChordProcessor::startRecording()
{
    if (recording.load())
        return {};

    auto dm = std::make_unique<juce::AudioDeviceManager>();
    const auto err = dm->initialiseWithDefaultDevices (1, 0);
    if (err.isNotEmpty())
        return "Could not open microphone: " + err;

    auto* dev = dm->getCurrentAudioDevice();
    if (dev == nullptr)
        return "No microphone found. Check Windows sound settings, or use 'Load audio file'.";

    recSampleRate = dev->getCurrentSampleRate();
    recBuffer.assign ((size_t) (recSampleRate * 90.0), 0.0f); // up to 90 seconds
    writePos.store (0);

    deviceManager = std::move (dm);
    deviceManager->addAudioCallback (this);
    recording.store (true);
    setStatus ("Recording... sing or hum your melody (max 90 seconds). Press Stop when done.");
    return {};
}

void VoxChordProcessor::stopRecording()
{
    if (! recording.load())
        return;

    deviceManager->removeAudioCallback (this);
    deviceManager->closeAudioDevice();
    deviceManager.reset();
    recording.store (false);

    const int n = writePos.load();
    auto data = std::make_shared<std::vector<float>> (recBuffer.begin(), recBuffer.begin() + n);
    analyseAsync (data, recSampleRate);
}

void VoxChordProcessor::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out,
                                                          int numOut, int numSamples,
                                                          const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOut; ++ch)
        if (out[ch] != nullptr)
            juce::FloatVectorOperations::clear (out[ch], numSamples);

    const int wp = writePos.load();
    const int n = juce::jmin (numSamples, (int) recBuffer.size() - wp);
    if (n <= 0)
        return;
    float blockPeak = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float s = 0.0f;
        int c = 0;
        for (int ch = 0; ch < numIn; ++ch)
            if (in[ch] != nullptr) { s += in[ch][i]; ++c; }
        const float v = c > 0 ? s / (float) c : 0.0f;
        recBuffer[(size_t) (wp + i)] = v;
        blockPeak = juce::jmax (blockPeak, std::abs (v));
    }
    inputPeak.store (juce::jmax (blockPeak, inputPeak.load() * 0.9f));
    writePos.store (wp + n);
}

//==============================================================================
// Loading an audio file (wav, mp3, aiff, flac, ogg)
//==============================================================================
void VoxChordProcessor::loadAudioFile (const juce::File& file)
{
    setStatus ("Reading " + file.getFileName() + "...");

    pool.addJob ([this, file]
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr)
        {
            setStatus ("Could not read that file. Try a WAV or MP3.");
            return;
        }

        const double sr = reader->sampleRate;
        const int n = (int) juce::jmin ((juce::int64) (sr * 180.0), reader->lengthInSamples); // max 3 minutes
        juce::AudioBuffer<float> buf ((int) juce::jmax (1u, reader->numChannels), n);
        reader->read (&buf, 0, n, 0, true, true);

        auto mono = std::make_shared<std::vector<float>> ((size_t) n, 0.0f);
        const int chans = buf.getNumChannels();
        for (int ch = 0; ch < chans; ++ch)
        {
            const float* r = buf.getReadPointer (ch);
            for (int i = 0; i < n; ++i)
                (*mono)[(size_t) i] += r[i] / (float) chans;
        }

        runAnalysis (*mono, sr);
    });
}

void VoxChordProcessor::analyseAsync (std::shared_ptr<std::vector<float>> mono, double sr)
{
    setStatus ("Analysing your melody...");

    pool.addJob ([this, mono, sr]
    {
        runAnalysis (*mono, sr);
    });
}

void VoxChordProcessor::runAnalysis (const std::vector<float>& mono, double sr)
{
    vc::AnalysisStats st;
    auto frames = vc::trackPitch (mono, sr, &st);
    auto raw = vc::segmentNotes (frames);

    juce::String d;
    if (st.peak < 1.0e-4f)
        d = "The microphone recorded pure silence. In Windows Settings > Privacy & security > Microphone, "
            "allow desktop apps to use the microphone, and check your default input device.";
    else if (st.durationSec < 0.5)
        d = "That recording was too short. Hold Record for a few seconds while you sing.";
    else if (st.voiced == 0)
        d = "Recorded " + juce::String (st.durationSec, 1) + " s but heard no clear pitch. Sing louder and closer "
            "to the mic, using steady 'la' or 'ooh' sounds.";
    else
        d = "Heard pitch in " + juce::String (st.voiced * 100 / juce::jmax (1, st.frames))
            + "% of the recording but no stable notes. Hold each note a little longer.";

    {
        const juce::ScopedLock sl (dataLock);
        rawNotes = raw;
        diag = d;
    }
    rebuild();
}

//==============================================================================
// Raw notes -> quantised notes in beats -> key -> chords
//==============================================================================
void VoxChordProcessor::rebuild()
{
    const double bpm = (double) apvts.getRawParameterValue ("bpm")->load();
    const int gridIdx = (int) apvts.getRawParameterValue ("grid")->load();
    const int lenIdx = (int) apvts.getRawParameterValue ("chordLen")->load();

    static const double grids[] = { 0.125, 0.25, 0.5, 1.0, 0.0 };
    static const double segs[] = { 2.0, 4.0, 8.0 };

    const juce::ScopedLock sl (dataLock);

    notes = vc::toBeats (rawNotes, bpm, grids[juce::jlimit (0, 4, gridIdx)]);
    key = vc::detectKey (notes);
    if (apvts.getRawParameterValue ("snap")->load() > 0.5f)
        vc::snapToKey (notes, key);
    chords = vc::makeChords (notes, key, segs[juce::jlimit (0, 2, lenIdx)]);

    double end = 0.0;
    for (const auto& n : notes) end = std::max (end, n.startBeat + n.durBeat);
    for (const auto& c : chords) end = std::max (end, c.startBeat + c.durBeat);
    loopBeats = notes.empty() ? 0.0 : std::max (4.0, std::ceil (end / 4.0 - 1.0e-9) * 4.0);

    if (notes.empty())
        status = rawNotes.empty() ? (diag.isNotEmpty() ? diag : juce::String ("No notes found."))
                                  : "No notes after quantise. Try a finer grid.";
    else
        status = juce::String ((int) notes.size()) + " notes found. Key: " + juce::String (key.name)
                 + ". Press Play here, or play your FL Studio project.";
}

//==============================================================================
// Real-time playback of the notes / chords as MIDI, locked to the host transport
//==============================================================================
void VoxChordProcessor::releaseAllNotes (juce::MidiBuffer& midi, int samplePos)
{
    for (int c = 0; c < 2; ++c)
        for (int n = 0; n < 128; ++n)
            if (sounding[c][n])
            {
                midi.addEvent (juce::MidiMessage::noteOff (c + 1, n), samplePos);
                sounding[c][n] = false;
            }
}

void VoxChordProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();
    midi.clear();

    const bool wantMel = apvts.getRawParameterValue ("melody")->load() > 0.5f;
    const bool wantChd = apvts.getRawParameterValue ("chords")->load() > 0.5f;
    const bool wantPreview = apvts.getRawParameterValue ("preview")->load() > 0.5f;
    const int transpose = (int) apvts.getRawParameterValue ("transpose")->load();

    double bpm = 120.0, ppq0 = 0.0;
    bool playing = false;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto b = pos->getBpm()) bpm = *b;
            if (auto p = pos->getPpqPosition()) ppq0 = *p; else playing = false;
        }
    }
    if (bpm < 20.0) bpm = 120.0;
    hostBpm.store (bpm);

    // built-in "Play" button: runs on its own clock when the host is stopped
    if (! playing && auditioning.load())
    {
        bpm = (double) apvts.getRawParameterValue ("bpm")->load();
        if (auditionRestart.exchange (false)) { auditionPos = 0.0; lastPpqEnd = -1.0; }
        ppq0 = auditionPos;
        auditionPos += (double) n / (currentSampleRate * 60.0 / bpm);
        playing = true;
        if (loopBeats > 0.0 && ppq0 >= loopBeats)
        {
            auditioning.store (false);
            playing = false;
        }
    }

    // settings changed while notes are sounding -> let go of everything cleanly
    if (transpose != lastTranspose || wantMel != lastMel || wantChd != lastChd)
        releaseAllNotes (midi, 0);
    lastTranspose = transpose; lastMel = wantMel; lastChd = wantChd;

    const juce::CriticalSection::ScopedTryLockType lock (dataLock);

    if (! lock.isLocked())
    {
        lockMissed = true; // couldn't read the data this block; clean up on the next one
    }
    else if (! playing || loopBeats <= 0.0)
    {
        releaseAllNotes (midi, 0);
        lastPpqEnd = -1.0;
        lockMissed = false;
    }
    else
    {
        const double samplesPerBeat = currentSampleRate * 60.0 / bpm;
        const double ppq1 = ppq0 + (double) n / samplesPerBeat;

        // transport jumped (seek / loop) or a block was skipped -> release hanging notes
        if (lockMissed || lastPpqEnd < 0.0 || std::abs (ppq0 - lastPpqEnd) > 0.25)
            releaseAllNotes (midi, 0);
        lockMissed = false;
        lastPpqEnd = ppq1;

        // time (in beats) of the next occurrence of loop-position e inside this block, or -1
        auto occurrence = [&] (double e) -> double
        {
            const double k = std::ceil ((ppq0 - e) / loopBeats - 1.0e-9);
            const double t = e + k * loopBeats;
            return (t >= ppq0 - 1.0e-9 && t < ppq1) ? t : -1.0;
        };
        auto sampleOf = [&] (double t)
        {
            return juce::jlimit (0, juce::jmax (0, n - 1), (int) std::floor ((t - ppq0) * samplesPerBeat));
        };

        // pass 1: note-offs (so a retriggered note gets off-then-on)
        if (wantMel)
            for (const auto& nt : notes)
            {
                const int num = juce::jlimit (0, 127, nt.midi + transpose);
                const double t = occurrence (nt.startBeat + nt.durBeat);
                if (t >= 0.0 && sounding[0][num])
                {
                    midi.addEvent (juce::MidiMessage::noteOff (1, num), sampleOf (t));
                    sounding[0][num] = false;
                }
            }
        if (wantChd)
            for (const auto& ch : chords)
            {
                const double t = occurrence (ch.startBeat + ch.durBeat);
                if (t < 0.0) continue;
                for (int num : ch.notes)
                    if (sounding[1][num])
                    {
                        midi.addEvent (juce::MidiMessage::noteOff (2, num), sampleOf (t));
                        sounding[1][num] = false;
                    }
            }

        // pass 2: note-ons
        if (wantMel)
            for (const auto& nt : notes)
            {
                const double t = occurrence (nt.startBeat);
                if (t < 0.0) continue;
                const int num = juce::jlimit (0, 127, nt.midi + transpose);
                midi.addEvent (juce::MidiMessage::noteOn (1, num, (juce::uint8) nt.vel), sampleOf (t));
                sounding[0][num] = true;
            }
        if (wantChd)
            for (const auto& ch : chords)
            {
                const double t = occurrence (ch.startBeat);
                if (t < 0.0) continue;
                for (int num : ch.notes)
                {
                    midi.addEvent (juce::MidiMessage::noteOn (2, num, (juce::uint8) 80), sampleOf (t));
                    sounding[1][num] = true;
                }
            }
    }

    // built-in preview sound
    if (wantPreview)
        synth.renderNextBlock (buffer, midi, 0, n);
    else if (lastPreview)
        synth.allNotesOff (0, false);
    lastPreview = wantPreview;
}

//==============================================================================
// MIDI file export (drag the .mid into FL Studio's piano roll or playlist)
//==============================================================================
bool VoxChordProcessor::exportMidi (const juce::File& file, int part, bool announce)
{
    std::vector<vc::Note> n;
    std::vector<vc::Chord> c;
    {
        const juce::ScopedLock sl (dataLock);
        n = notes;
        c = chords;
    }
    if (n.empty())
    {
        setStatus ("Nothing to export yet. Record or load a melody first.");
        return false;
    }

    const double bpm = (double) apvts.getRawParameterValue ("bpm")->load();
    const int transpose = (int) apvts.getRawParameterValue ("transpose")->load();
    constexpr int ppq = 960;

    juce::MidiMessageSequence melody, harmony;
    {
        auto tempo = juce::MidiMessage::tempoMetaEvent ((int) (60000000.0 / bpm));
        tempo.setTimeStamp (0.0);
        melody.addEvent (tempo);
        if (part == 2)
        {
            auto t2 = juce::MidiMessage::tempoMetaEvent ((int) (60000000.0 / bpm));
            t2.setTimeStamp (0.0);
            harmony.addEvent (t2);
        }
    }

    auto addNote = [ppq] (juce::MidiMessageSequence& seq, int ch, int num, int vel, double startBeat, double durBeat)
    {
        auto on = juce::MidiMessage::noteOn (ch, num, (juce::uint8) vel);
        on.setTimeStamp (startBeat * ppq);
        auto off = juce::MidiMessage::noteOff (ch, num);
        off.setTimeStamp ((startBeat + durBeat) * ppq - 1.0);
        seq.addEvent (on);
        seq.addEvent (off);
    };

    if (part != 2)
        for (const auto& nt : n)
            addNote (melody, 1, juce::jlimit (0, 127, nt.midi + transpose), nt.vel, nt.startBeat, nt.durBeat);
    if (part != 1)
        for (const auto& ch : c)
            for (int num : ch.notes)
                addNote (harmony, 2, num, 80, ch.startBeat, ch.durBeat);

    melody.sort();
    harmony.sort();
    melody.updateMatchedPairs();
    harmony.updateMatchedPairs();

    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (ppq);
    if (part != 2) mf.addTrack (melody);
    if (part != 1) mf.addTrack (harmony);

    file.deleteFile();
    juce::FileOutputStream os (file);
    if (! os.openedOk())
    {
        setStatus ("Could not write the file. Pick another folder.");
        return false;
    }
    mf.writeTo (os);
    if (announce)
        setStatus ("Saved " + file.getFileName() + " (track 1 = melody, track 2 = chords).");
    return true;
}

//==============================================================================
// Saving / loading the project state (so your melody survives closing FL Studio)
//==============================================================================
void VoxChordProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());

    juce::String s;
    {
        const juce::ScopedLock sl (dataLock);
        for (const auto& r : rawNotes)
            s << juce::String (r.startSec, 4) << "," << juce::String (r.durSec, 4) << "," << r.midi << "," << r.vel << ";";
    }
    xml->setAttribute ("rawNotes", s);
    copyXmlToBinary (*xml, dest);
}

void VoxChordProcessor::setStateInformation (const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, size));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    std::vector<vc::RawNote> raw;
    juce::StringArray items;
    items.addTokens (xml->getStringAttribute ("rawNotes"), ";", "");
    for (const auto& it : items)
    {
        juce::StringArray f;
        f.addTokens (it, ",", "");
        if (f.size() == 4)
            raw.push_back ({ f[0].getDoubleValue(), f[1].getDoubleValue(), f[2].getIntValue(), f[3].getIntValue() });
    }

    apvts.replaceState (juce::ValueTree::fromXml (*xml));
    {
        const juce::ScopedLock sl (dataLock);
        rawNotes = raw;
    }
    rebuild();
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VoxChordProcessor();
}
