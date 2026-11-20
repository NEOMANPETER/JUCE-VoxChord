#include "../Source/Analysis.h"
#include <cstdio>
#include <random>

static double midiToHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

int main()
{
    const double sr = 48000.0;
    const int melody[] = { 62, 66, 69, 66, 64, 67, 71, 67, 62, 66, 69, 74 };  // D major-ish tune
    std::mt19937 rng (7);
    std::normal_distribution<float> hiss (0.0f, 1.0f);

    std::vector<float> a;
    const size_t lead = (size_t) (sr * 1.0);
    for (size_t i = 0; i < lead; ++i) a.push_back (0.003f * hiss (rng));
    // loud click from pressing the Record button
    for (size_t i = 0; i < (size_t) (sr * 0.005); ++i) a[i + 2000] = 0.9f * (i % 2 ? 1.0f : -1.0f);

    double phase = 0.0;
    for (int m : melody)
    {
        const double noteLen = 0.45;
        const size_t n = (size_t) (sr * noteLen);
        const double drift = 0.30;                      // sings 30 cents sharp
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            const double scoop = (t < 0.06) ? -1.0 * (1.0 - t / 0.06) : 0.0;   // slides up from 1 semitone below
            const double vibDepth = std::min (1.0, t / 0.2) * 0.25;             // growing vibrato +-25 cents
            const double pitch = m + drift + scoop + vibDepth * std::sin (2.0 * M_PI * 5.8 * t);
            phase += 2.0 * M_PI * midiToHz (pitch) / sr;
            const double env = std::min (1.0, std::min (t / 0.05, (noteLen - t) / 0.06));
            double v = std::sin (phase) + 0.45 * std::sin (2 * phase) + 0.25 * std::sin (3 * phase) + 0.1 * std::sin (4 * phase);
            const double rumble = 0.012 * std::sin (2.0 * M_PI * 50.0 * (double) a.size() / sr);   // 50 Hz hum
            a.push_back ((float) (0.03 * env * v) + 0.004f * hiss (rng) + (float) rumble);        // quiet: peak ~0.05
        }
        for (size_t i = 0; i < (size_t) (sr * 0.06); ++i) a.push_back (0.004f * hiss (rng));
    }
    for (size_t i = 0; i < (size_t) (sr * 0.6); ++i) a.push_back (0.003f * hiss (rng));

    vc::AnalysisStats st;
    auto frames = vc::trackPitch (a, sr, &st);
    auto raw = vc::segmentNotes (frames);
    auto notes = vc::toBeats (raw, 120.0, 0.25);
    auto key = vc::detectKey (notes);
    vc::snapToKey (notes, key);
    auto chords = vc::makeChords (notes, key, 4.0);

    std::printf ("duration %.1fs peak %.3f frames %d voiced %d  raw notes %zu\n", st.durationSec, st.peak, st.frames, st.voiced, raw.size());
    std::printf ("key: %s\n", key.name.c_str());
    int ok = 0;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const bool match = i < 12 && notes[i].midi == melody[i];
        ok += match;
        std::printf ("  %2zu: midi %d (want %d) %s\n", i, notes[i].midi, i < 12 ? melody[i] : -1, match ? "OK" : "<--");
    }
    for (auto& c : chords) std::printf ("  chord %.0f: %s\n", c.startBeat, c.name.c_str());
    std::printf ("matched %d/12, detected %zu notes\n", ok, notes.size());
    return ok >= 10 ? 0 : 1;
}
