#pragma once
// RoomTamer DSP core: lays one steady room tone under the whole programme so
// pauses never fall to dead digital silence, at a level ACX accepts, and meters
// the result the way ACX measures it.
//
//   1. Room tone: Gaussian noise, pink-shaped, with a 40 Hz high-pass, a soft
//      low-end lift (the rumble of a real room) and a gentle low-pass (Tone sets
//      it), like a quiet booth rather than raw hiss.
//      The same signal goes to every channel. Its RMS equals Level exactly: the
//      filter chain's power is integrated from its frequency response.
//   2. Repeatable: each noise sample is a hash of its timeline position, so a
//      render of the same range always produces the same room tone. After a
//      jump the filters are warmed up on the samples just before the new spot.
//   3. ACX meter on the output: noise floor (RMS of the quietest 500 ms),
//      overall RMS and peak, since the meter was last reset.
//
// Plain C++17, shared by the plugin, the standalone app and the CLI.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace rt
{
struct Params
{
    float levelDb = -65.0f; // room tone RMS, dBFS (ACX noise floor limit is -60)
    float tone    = 0.0f;   // -100 (darker) .. 100 (brighter)
    bool  bypass  = false;
};

// ACX audio submission limits.
constexpr float kFloorMaxDb = -60.0f, kRmsMinDb = -23.0f, kRmsMaxDb = -18.0f, kPeakMaxDb = -3.0f;
constexpr float kDeadSilenceDb = -90.0f; // a floor this low is digital silence, which ACX rejects

struct Reading
{
    float floorDb = -200.0f, rmsDb = -200.0f, peakDb = -200.0f; // -200 = nothing measured / silence
    double seconds = 0.0;
    bool hasFloor = false; // a full 500 ms has been measured
};

// Pass/fail against the ACX limits, shared by every front end.
enum class Verdict { none, pass, fail };
inline Verdict floorVerdict (const Reading& r)
{
    if (! r.hasFloor)
        return Verdict::none;
    return r.floorDb <= kFloorMaxDb && r.floorDb > kDeadSilenceDb ? Verdict::pass : Verdict::fail;
}
inline Verdict rmsVerdict (const Reading& r)
{
    if (r.seconds <= 0.0)
        return Verdict::none;
    return r.rmsDb >= kRmsMinDb && r.rmsDb <= kRmsMaxDb ? Verdict::pass : Verdict::fail;
}
inline Verdict peakVerdict (const Reading& r)
{
    if (r.seconds <= 0.0)
        return Verdict::none;
    return r.peakDb <= kPeakMaxDb ? Verdict::pass : Verdict::fail;
}

class Engine
{
public:
    void prepare (double sampleRate, int maxChannels)
    {
        fs = sampleRate;
        numCh = std::max (1, maxChannels);
        hopLen = std::max (1, (int) std::lround (fs * 0.050));
        hp = design (40.0, 0.7071, true);
        shelf = lowShelf (150.0, 4.0);
        cachedTone = 1.0e9f; // force filter design
        updateShape (params.tone);
        gain = targetGain();
        reset();
    }

    // Restarts the generator at timeline position 0 and clears the meter.
    void reset()
    {
        next = 0;
        warmUp (0);
        resetMeterNow();
        resetRequest.store (false);
    }

    void setParams (const Params& p) { params = p; }
    void requestMeterReset() { resetRequest.store (true); } // safe from the UI thread
    Reading reading() const
    {
        Reading r;
        r.floorDb = floorOut.load();
        r.rmsDb = rmsOut.load();
        r.peakDb = peakOut.load();
        r.seconds = secondsOut.load();
        r.hasFloor = hasFloorOut.load();
        return r;
    }

    // Adds the room tone. `pos` is the timeline sample of the block's first
    // sample, or -1 when unknown (the generator then just runs on). `meter`
    // chooses whether this block counts towards the ACX reading.
    void process (float* const* ch, int nch, int ns, int64_t pos, bool meter)
    {
        nch = std::min (nch, numCh);
        if (nch <= 0)
            return;
        if (resetRequest.exchange (false))
            resetMeterNow();

        if (pos >= 0 && pos != next)
            warmUp (pos);
        if (std::abs (params.tone - cachedTone) > 0.01f)
            updateShape (params.tone);
        const double target = targetGain();
        const double smooth = 1.0 - std::exp (-1.0 / (0.02 * fs));

        for (int i = 0; i < ns; ++i)
        {
            gain += (target - gain) * smooth;
            const float room = (float) (gain * shaped (next++));
            double pw = 0.0, peak = 0.0;
            for (int c = 0; c < nch; ++c)
            {
                float& x = ch[c][i];
                if (! params.bypass)
                    x += room;
                pw += (double) x * x;
                peak = std::max (peak, (double) std::abs (x));
            }
            if (meter)
                measure (pw / nch, peak);
        }
        if (meter)
            publish();
    }

    // Power of the shaped noise for unit-variance white input (for tests).
    double shapePower() const { return power; }

private:
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; };

    Biquad design (double fc, double q, bool high) const
    {
        Biquad f;
        const double w0 = 6.283185307179586 * fc / fs, cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        f.b0 = (high ? (1.0 + cw) : (1.0 - cw)) / 2.0 / a0;
        f.b1 = (high ? -(1.0 + cw) : (1.0 - cw)) / a0;
        f.b2 = f.b0;
        f.a1 = -2.0 * cw / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }

    Biquad lowShelf (double fc, double gainDb) const
    {
        Biquad f;
        const double A = std::pow (10.0, gainDb / 40.0), w0 = 6.283185307179586 * fc / fs;
        const double cw = std::cos (w0), alpha = std::sin (w0) / 2.0 * std::sqrt (2.0), sa = 2.0 * std::sqrt (A) * alpha;
        const double a0 = (A + 1.0) + (A - 1.0) * cw + sa;
        f.b0 = A * ((A + 1.0) - (A - 1.0) * cw + sa) / a0;
        f.b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw) / a0;
        f.b2 = A * ((A + 1.0) - (A - 1.0) * cw - sa) / a0;
        f.a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw) / a0;
        f.a2 = ((A + 1.0) + (A - 1.0) * cw - sa) / a0;
        return f;
    }

    static double run (Biquad& f, double x)
    {
        const double y = f.b0 * x + f.z1;
        f.z1 = f.b1 * x - f.a1 * y + f.z2;
        f.z2 = f.b2 * x - f.a2 * y;
        return y;
    }

    // ---- noise: white sample n is a pure function of n ----
    static uint64_t mix (uint64_t z)
    {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    static double white (int64_t n)
    {
        const uint64_t h = mix ((uint64_t) n ^ 0x5243544D52ull); // "RTMR"
        const double u1 = ((double) (h >> 11) + 0.5) * (1.0 / 9007199254740992.0);
        const double u2 = ((double) (mix (h) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
        return std::sqrt (-2.0 * std::log (u1)) * std::cos (6.283185307179586 * u2); // Box-Muller
    }

    // Pink filter (Paul Kellet's refined method), then the booth band-limits.
    static constexpr double pa[6] = { 0.99886, 0.99332, 0.96900, 0.86650, 0.55000, -0.7616 };
    static constexpr double pc[6] = { 0.0555179, 0.0750759, 0.1538520, 0.3104856, 0.5329522, -0.0168980 };
    static constexpr double pDirect = 0.5362, pDelayed = 0.115926;

    double shaped (int64_t n)
    {
        const double w = white (n);
        double pink = pDirect * w + pDelayed * prevWhite;
        for (int k = 0; k < 6; ++k)
        {
            pinkState[k] = pa[k] * pinkState[k] + pc[k] * w;
            pink += pinkState[k];
        }
        prevWhite = w;
        return run (lp, run (shelf, run (hp, pink)));
    }

    // Clears the filters and runs them over the samples just before `pos`, so
    // the room tone at `pos` is the same however playback got there.
    void warmUp (int64_t pos)
    {
        std::fill (std::begin (pinkState), std::end (pinkState), 0.0);
        prevWhite = 0.0;
        hp.z1 = hp.z2 = shelf.z1 = shelf.z2 = lp.z1 = lp.z2 = 0.0;
        next = pos - kWarmUp;
        while (next < pos)
            shaped (next++);
    }

    void updateShape (float tone)
    {
        cachedTone = tone;
        const double t = std::clamp ((double) tone, -100.0, 100.0) / 100.0;
        const double fc = std::min (8000.0 * std::pow (2.0, 1.2 * t), 0.45 * fs);
        const Biquad old = lp;
        lp = design (fc, 0.7071, false);
        lp.z1 = old.z1;
        lp.z2 = old.z2;
        power = chainPower();
    }

    // Output variance of the whole chain for unit white noise: the mean of
    // |H|^2 over 0..pi (Parseval).
    double chainPower() const
    {
        using C = std::complex<double>;
        constexpr int N = 16384;
        double sum = 0.0;
        for (int k = 0; k < N; ++k)
        {
            const double w = 3.141592653589793 * (k + 0.5) / N;
            const C z1 = std::polar (1.0, -w), z2 = z1 * z1;
            C h = pDirect + pDelayed * z1;
            for (int i = 0; i < 6; ++i)
                h += pc[i] / (1.0 - pa[i] * z1);
            for (const Biquad* f : { &hp, &shelf, &lp })
                h *= (f->b0 + f->b1 * z1 + f->b2 * z2) / (1.0 + f->a1 * z1 + f->a2 * z2);
            sum += std::norm (h);
        }
        return sum / N;
    }

    double targetGain() const { return std::pow (10.0, params.levelDb / 20.0) / std::sqrt (power); }

    // ---- ACX meter ----
    void resetMeterNow()
    {
        sumSq = 0.0;
        count = 0;
        peakMax = 0.0;
        hopSum = 0.0;
        hopFill = 0;
        hopsSeen = 0;
        std::fill (std::begin (hops), std::end (hops), 0.0);
        floorMin = -1.0;
        publish();
    }

    void measure (double pw, double peak)
    {
        sumSq += pw;
        ++count;
        peakMax = std::max (peakMax, peak);
        hopSum += pw;
        if (++hopFill < hopLen)
            return;
        // A 500 ms window = the last ten 50 ms hops.
        hops[hopsSeen % 10] = hopSum;
        ++hopsSeen;
        hopSum = 0.0;
        hopFill = 0;
        if (hopsSeen >= 10)
        {
            double win = 0.0;
            for (double h : hops)
                win += h;
            win /= 10.0 * hopLen;
            floorMin = floorMin < 0.0 ? win : std::min (floorMin, win);
        }
    }

    static float toDb (double power) { return power > 1.0e-20 ? (float) (10.0 * std::log10 (power)) : -200.0f; }

    void publish()
    {
        rmsOut.store (count > 0 ? toDb (sumSq / (double) count) : -200.0f);
        peakOut.store (peakMax > 0.0 ? toDb (peakMax * peakMax) : -200.0f);
        floorOut.store (floorMin >= 0.0 ? toDb (floorMin) : -200.0f);
        hasFloorOut.store (floorMin >= 0.0);
        secondsOut.store ((double) count / fs);
    }

    static constexpr int kWarmUp = 8192; // slowest pink pole decays to 1e-4 within this

    Params params;
    double fs = 44100.0, gain = 0.0, power = 1.0, prevWhite = 0.0;
    double pinkState[6] = {};
    Biquad hp, shelf, lp;
    float cachedTone = 1.0e9f;
    int numCh = 1, hopLen = 2205, hopFill = 0;
    int64_t next = 0, count = 0, hopsSeen = 0;
    double sumSq = 0.0, peakMax = 0.0, hopSum = 0.0, floorMin = -1.0, hops[10] = {};
    std::atomic<bool> resetRequest { false }, hasFloorOut { false };
    std::atomic<float> floorOut { -200.0f }, rmsOut { -200.0f }, peakOut { -200.0f };
    std::atomic<double> secondsOut { 0.0 };
};
} // namespace rt
