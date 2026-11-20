#include "../Source/Analysis.h"
#include <cstdio>
#include <cstdlib>
#include <random>

static double midiToHz (int m) { return 440.0 * std::pow (2.0, (m - 69) / 12.0); }

int main()
{
    const double sr = 44100.0;
    // melody in C major: C4 E4 G4 E4 | F4 A4 C5 A4 | G4 B4 D5 B4 | C5 (held)   at 120 bpm, 0.5s per note
    const int melody[] = { 60, 64, 67, 64, 65, 69, 72, 69, 67, 71, 74, 71, 72 };
    const double noteLen = 0.5;

    std::vector<float> audio;
    audio.resize ((size_t) (sr * 0.7), 0.0f); // leading silence (should be trimmed)
    std::mt19937 rng (1);
    std::normal_distribution<float> noise (0.0f, 0.005f);
    double phase = 0.0;
    size_t idx = 0;
    for (int m : melody)
    {
        const size_t n = (size_t) (sr * noteLen);
        for (size_t i = 0; i < n; ++i, ++idx)
        {
            const double t = (double) i / sr;
            const double vib = 1.0 + 0.008 * std::sin (2.0 * M_PI * 5.5 * t);   // vibrato ~ +-14 cents
            phase += 2.0 * M_PI * midiToHz (m) * vib / sr;
            // voice-like: fundamental + harmonics, soft fade at edges
            double env = std::min (1.0, std::min (t / 0.03, (noteLen - t) / 0.03));
            float s = (float) (env * 0.4 * (std::sin (phase) + 0.5 * std::sin (2 * phase) + 0.3 * std::sin (3 * phase)));
            audio.push_back (s + noise (rng));
        }
        // short breath gap between notes
        for (int i = 0; i < (int) (sr * 0.02); ++i) audio.push_back (noise (rng));
    }
    audio.resize (audio.size() + (size_t) (sr * 0.5), 0.0f);

    auto frames = vc::trackPitch (audio, sr);
    auto raw = vc::segmentNotes (frames);
    auto notes = vc::toBeats (raw, 120.0, 0.25);
    auto key = vc::detectKey (notes);
    auto chords = vc::makeChords (notes, key, 4.0);

    std::printf ("frames: %zu, raw notes: %zu\n", frames.size(), raw.size());
    int ok = 0;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const bool match = i < sizeof (melody) / sizeof (int) && notes[i].midi == melody[i];
        ok += match;
        std::printf ("  note %2zu  midi %d (expected %d)  start %.2f beats  dur %.2f %s\n", i, notes[i].midi,
                     i < sizeof (melody) / sizeof (int) ? melody[i] : -1, notes[i].startBeat, notes[i].durBeat, match ? "OK" : "<-- MISMATCH");
    }
    std::printf ("notes correct: %d / %zu\n", ok, sizeof (melody) / sizeof (int));
    std::printf ("key: %s\n", key.name.c_str());
    for (auto& c : chords)
        std::printf ("  chord @ beat %.1f: %s  [%d %d %d]\n", c.startBeat, c.name.c_str(), c.notes[0], c.notes[1], c.notes[2]);

    const bool pass = (ok >= 11) && key.name == "C major" && ! chords.empty();
    std::printf (pass ? "TEST PASSED\n" : "TEST FAILED\n");
    return pass ? 0 : 1;
}
