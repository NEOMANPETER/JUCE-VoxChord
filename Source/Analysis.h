#pragma once
// VoxChord analysis engine. Plain C++17, no JUCE dependency, works fully offline.
//   audio -> pitch track (YIN) -> notes -> key -> chords

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace vc
{
struct RawNote { double startSec; double durSec; int midi; int vel; };
struct Note    { double startBeat; double durBeat; int midi; int vel; };
struct Chord   { double startBeat; double durBeat; std::vector<int> notes; std::string name; };
struct KeyInfo { int root = 0; bool minor = false; std::string name = "-"; };

inline const char* pcName (int pc)
{
    static const char* n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return n[((pc % 12) + 12) % 12];
}

struct PitchFrame { double timeSec; float midi; float rms; bool voiced; };

// ---------------------------------------------------------------------------
// 1. Pitch tracking (YIN algorithm) on a mono signal
// ---------------------------------------------------------------------------
struct AnalysisStats { double durationSec = 0.0; float peak = 0.0f; int frames = 0; int voiced = 0; };

inline std::vector<PitchFrame> trackPitch (const std::vector<float>& input, double sr, AnalysisStats* stats = nullptr)
{
    std::vector<PitchFrame> out;
    if (stats) *stats = AnalysisStats();
    if (sr <= 0.0)
        return out;

    float peak = 0.0f;
    for (float s : input) peak = std::max (peak, std::fabs (s));
    if (stats) { stats->durationSec = (double) input.size() / sr; stats->peak = peak; }
    if (input.size() < 4096 || peak < 1.0e-4f)
        return out;

    // decimate to ~12 kHz (box filter) to make analysis fast
    const int dec = std::max (1, (int) std::lround (sr / 12000.0));
    const double dsr = sr / dec;
    std::vector<float> x (input.size() / (size_t) dec);
    for (size_t i = 0; i < x.size(); ++i)
    {
        float acc = 0.0f;
        for (int k = 0; k < dec; ++k) acc += input[i * (size_t) dec + (size_t) k];
        x[i] = acc / (float) dec;
    }

    // high-pass (~50 Hz) removes DC offset, desk thumps and mains rumble
    {
        float px = 0.0f, py = 0.0f;
        const float R = 0.975f;
        for (auto& v : x) { const float in = v; const float y = in - px + R * py; px = in; py = y; v = y; }
    }

    const int W = 512;
    const int tauMin = std::max (2, (int) (dsr / 1000.0));
    const int tauMax = (int) (dsr / 70.0);
    const int hop = 128;
    const int need = W + tauMax + 1;
    if ((int) x.size() < need)
        return out;

    // pass 1: loudness of every frame, to set a gate relative to how loud YOU sing
    std::vector<float> frameRms;
    for (size_t pos = 0; pos + (size_t) need <= x.size(); pos += (size_t) hop)
    {
        double e = 0.0;
        for (int j = 0; j < W; ++j) e += (double) x[pos + (size_t) j] * x[pos + (size_t) j];
        frameRms.push_back ((float) std::sqrt (e / W));
    }
    std::vector<float> sorted = frameRms;
    std::sort (sorted.begin(), sorted.end());
    const float p90 = sorted[(size_t) ((double) sorted.size() * 0.9)];
    const float gate = std::max (0.0012f, 0.12f * p90);

    std::vector<double> d ((size_t) tauMax + 1), cm ((size_t) tauMax + 1);
    size_t fi = 0;
    for (size_t pos = 0; pos + (size_t) need <= x.size(); pos += (size_t) hop, ++fi)
    {
        const float* f = &x[pos];
        const float rms = frameRms[fi];

        PitchFrame pf { ((double) pos + W / 2.0) * dec / sr, 0.0f, rms, false };

        if (rms > gate)
        {
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                double s = 0.0;
                for (int j = 0; j < W; ++j)
                {
                    const double diff = (double) f[j] - (double) f[j + tau];
                    s += diff * diff;
                }
                d[(size_t) tau] = s;
            }

            cm[0] = 1.0;
            double run = 0.0;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                run += d[(size_t) tau];
                cm[(size_t) tau] = run > 0.0 ? d[(size_t) tau] * tau / run : 1.0;
            }

            int best = -1;
            for (int tau = tauMin; tau <= tauMax; ++tau)
            {
                if (cm[(size_t) tau] < 0.20)
                {
                    while (tau + 1 <= tauMax && cm[(size_t) tau + 1] < cm[(size_t) tau]) ++tau;
                    best = tau;
                    break;
                }
            }
            if (best < 0)
            {
                int mt = tauMin;
                for (int tau = tauMin; tau <= tauMax; ++tau)
                    if (cm[(size_t) tau] < cm[(size_t) mt]) mt = tau;
                if (cm[(size_t) mt] < 0.40) best = mt;
            }

            if (best > 0)
            {
                double t = best;
                if (best > 1 && best < tauMax)
                {
                    const double a = cm[(size_t) best - 1], b = cm[(size_t) best], c = cm[(size_t) best + 1];
                    const double den = a - 2.0 * b + c;
                    if (std::fabs (den) > 1.0e-12) t = best + 0.5 * (a - c) / den;
                }
                const double f0 = dsr / t;
                if (f0 >= 70.0 && f0 <= 1000.0)
                {
                    pf.midi = (float) (69.0 + 12.0 * std::log2 (f0 / 440.0));
                    pf.voiced = true;
                }
            }
        }
        if (stats) { stats->frames++; if (pf.voiced) stats->voiced++; }
        out.push_back (pf);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 2. Pitch track -> notes (in seconds)
// ---------------------------------------------------------------------------
inline std::vector<RawNote> segmentNotes (const std::vector<PitchFrame>& frames)
{
    std::vector<RawNote> notes;
    if (frames.size() < 3)
        return notes;

    const double hop = frames[1].timeSec - frames[0].timeSec;

    // median smoothing over +-2 frames (voiced frames only)
    std::vector<float> sm (frames.size(), 0.0f);
    for (size_t i = 0; i < frames.size(); ++i)
    {
        if (! frames[i].voiced) continue;
        std::vector<float> w;
        for (long k = (long) i - 2; k <= (long) i + 2; ++k)
            if (k >= 0 && k < (long) frames.size() && frames[(size_t) k].voiced)
                w.push_back (frames[(size_t) k].midi);
        std::sort (w.begin(), w.end());
        sm[i] = w[w.size() / 2];
    }

    bool active = false;
    double start = 0.0, lastT = 0.0, rmsSum = 0.0;
    int gap = 0;
    std::vector<float> vals;

    auto medianOf = [] (std::vector<float> v)
    {
        std::nth_element (v.begin(), v.begin() + (long) (v.size() / 2), v.end());
        return v[v.size() / 2];
    };

    auto closeNote = [&]()
    {
        if (active && ! vals.empty())
        {
            const double dur = lastT - start + hop;
            if (dur >= 0.07)
            {
                const int m = (int) std::lround (medianOf (vals));
                const float rms = (float) (rmsSum / (double) vals.size());
                const int vel = std::clamp ((int) (50.0f + rms * 600.0f), 55, 120);
                notes.push_back ({ std::max (0.0, start), dur, m, vel });
            }
        }
        active = false;
        vals.clear();
        rmsSum = 0.0;
        gap = 0;
    };

    for (size_t i = 0; i < frames.size(); ++i)
    {
        if (frames[i].voiced)
        {
            const float m = sm[i];
            if (active && std::fabs (m - medianOf (vals)) > 0.8f)
                closeNote();

            if (! active)
            {
                active = true;
                start = frames[i].timeSec - hop / 2.0;
                vals.clear();
                rmsSum = 0.0;
            }
            vals.push_back (m);
            rmsSum += frames[i].rms;
            lastT = frames[i].timeSec;
            gap = 0;
        }
        else if (active)
        {
            if (++gap >= 3) closeNote();
        }
    }
    closeNote();

    // merge same-pitch notes separated by a tiny gap
    std::vector<RawNote> merged;
    for (const auto& n : notes)
    {
        if (! merged.empty() && merged.back().midi == n.midi
            && n.startSec - (merged.back().startSec + merged.back().durSec) < 0.06)
            merged.back().durSec = n.startSec + n.durSec - merged.back().startSec;
        else
            merged.push_back (n);
    }
    return merged;
}

// ---------------------------------------------------------------------------
// 3. Seconds -> beats, with optional quantise (grid in beats, 0 = off)
// ---------------------------------------------------------------------------
inline std::vector<Note> toBeats (const std::vector<RawNote>& raw, double bpm, double grid)
{
    std::vector<Note> out;
    if (raw.empty() || bpm <= 0.0) return out;

    const double t0 = raw.front().startSec; // trim leading silence
    for (const auto& r : raw)
    {
        double s = (r.startSec - t0) * bpm / 60.0;
        double e = s + r.durSec * bpm / 60.0;
        if (grid > 0.0)
        {
            s = std::round (s / grid) * grid;
            e = std::round (e / grid) * grid;
            if (e <= s) e = s + grid;
        }
        if (! out.empty() && s < out.back().startBeat + out.back().durBeat)
        {
            out.back().durBeat = s - out.back().startBeat;
            if (out.back().durBeat <= 1.0e-6) out.pop_back();
        }
        out.push_back ({ s, std::max (0.05, e - s), r.midi, r.vel });
    }
    return out;
}

// ---------------------------------------------------------------------------
// 4. Key detection (Krumhansl-Schmuckler)
// ---------------------------------------------------------------------------
inline KeyInfo detectKey (const std::vector<Note>& notes)
{
    KeyInfo k;
    double h[12] = {};
    double total = 0.0;
    for (const auto& n : notes) { h[((n.midi % 12) + 12) % 12] += n.durBeat; total += n.durBeat; }
    if (total <= 0.0) return k;

    static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double min[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    double mh = 0.0;
    for (double v : h) mh += v;
    mh /= 12.0;

    double bestScore = -2.0;
    for (int minor = 0; minor < 2; ++minor)
    {
        const double* p = minor ? min : maj;
        double mp = 0.0;
        for (int i = 0; i < 12; ++i) mp += p[i];
        mp /= 12.0;

        for (int r = 0; r < 12; ++r)
        {
            double num = 0.0, dh = 0.0, dp = 0.0;
            for (int i = 0; i < 12; ++i)
            {
                const double a = h[(i + r) % 12] - mh, b = p[i] - mp;
                num += a * b; dh += a * a; dp += b * b;
            }
            const double score = (dh > 0.0 && dp > 0.0) ? num / std::sqrt (dh * dp) : 0.0;
            if (score > bestScore)
            {
                bestScore = score;
                k.root = r;
                k.minor = (minor == 1);
            }
        }
    }
    k.name = std::string (pcName (k.root)) + (k.minor ? " minor" : " major");
    return k;
}

// ---------------------------------------------------------------------------
// 4b. Snap every note to the nearest note of the detected scale
//     (fixes small tuning slips, like the "key quantizer" in voice-to-MIDI tools)
// ---------------------------------------------------------------------------
inline void snapToKey (std::vector<Note>& notes, const KeyInfo& key)
{
    static const bool majS[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
    static const bool minS[12] = { 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0 };
    const bool* sc = key.minor ? minS : majS;
    auto inScale = [&] (int m) { return sc[(((m - key.root) % 12) + 12) % 12]; };
    for (auto& n : notes)
        for (int d = 0; d <= 6; ++d)
        {
            if (inScale (n.midi - d)) { n.midi -= d; break; }
            if (inScale (n.midi + d)) { n.midi += d; break; }
        }
}

// ---------------------------------------------------------------------------
// 5. Chord suggestion: best diatonic triad per segment
// ---------------------------------------------------------------------------
inline std::vector<Chord> makeChords (const std::vector<Note>& notes, const KeyInfo& key, double segBeats)
{
    std::vector<Chord> chords;
    if (notes.empty() || segBeats <= 0.0) return chords;

    static const int  majOff[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static const int  majQ[7]   = { 0, 1, 1, 0, 0, 1, 2 };   // 0 major, 1 minor, 2 diminished
    static const int  minOff[7] = { 0, 2, 3, 5, 7, 8, 10 };
    static const int  minQ[7]   = { 1, 2, 0, 1, 1, 0, 0 };
    static const double majBonus[7] = { 0.15, 0.00, 0.00, 0.10, 0.12, 0.08, 0.0 };
    static const double minBonus[7] = { 0.15, 0.00, 0.06, 0.10, 0.10, 0.06, 0.06 };

    const int* off = key.minor ? minOff : majOff;
    const int* qual = key.minor ? minQ : majQ;
    const double* bonus = key.minor ? minBonus : majBonus;

    double end = 0.0;
    for (const auto& n : notes) end = std::max (end, n.startBeat + n.durBeat);
    const int nseg = (int) std::ceil (end / segBeats - 1.0e-9);

    bool havePrev = false;
    Chord prev;

    for (int s = 0; s < nseg; ++s)
    {
        const double a = s * segBeats, b = a + segBeats;
        double w[12] = {};
        double total = 0.0;
        for (const auto& n : notes)
        {
            const double o = std::min (b, n.startBeat + n.durBeat) - std::max (a, n.startBeat);
            if (o > 0.0) { w[((n.midi % 12) + 12) % 12] += o; total += o; }
        }

        Chord c;
        c.startBeat = a;
        c.durBeat = segBeats;

        if (total <= 0.0)
        {
            if (! havePrev) continue;
            c.notes = prev.notes;
            c.name = prev.name;
        }
        else
        {
            int bestI = 0;
            double bestScore = -1.0e9;
            for (int i = 0; i < 7; ++i)
            {
                const int root = (key.root + off[i]) % 12;
                const int third = (root + (qual[i] == 0 ? 4 : 3)) % 12;
                const int fifth = (root + (qual[i] == 2 ? 6 : 7)) % 12;
                const double inChord = w[root] + 0.8 * w[third] + 0.7 * w[fifth];
                const double outChord = total - (w[root] + w[third] + w[fifth]);
                double score = inChord - 0.3 * outChord + bonus[i] * total;
                if (qual[i] == 2) score -= 0.2 * total;
                if (score > bestScore) { bestScore = score; bestI = i; }
            }
            const int root = (key.root + off[bestI]) % 12;
            const int q = qual[bestI];
            const int base = 48 + root;                       // chord sits around C3..B3
            c.notes = { base, base + (q == 0 ? 4 : 3), base + (q == 2 ? 6 : 7) };
            c.name = std::string (pcName (root)) + (q == 1 ? "m" : q == 2 ? "dim" : "");
        }
        prev = c;
        havePrev = true;
        chords.push_back (c);
    }
    return chords;
}

} // namespace vc
