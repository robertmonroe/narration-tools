#pragma once
// PopTamer DSP core: finds plosive pops (the low thump of "p", "b", "t", "k" air
// blasts) and reduces only the low end of each pop, keeping the voice intact.
//
//   1. Detection follows the 20-80 Hz band. A pop is a short burst (<= 120 ms)
//      far above what the voice explains there: the band's local level (median
//      over +-150 ms) plus the voice's fundamental band (100-300 Hz) shifted by
//      this voice's usual low-end-to-fundamental ratio. Comparing with the
//      fundamental at the same moment keeps pops at word onsets (where "p" and
//      "b" happen) detectable while normal low vowels are not.
//   2. Each pop is reduced below the frequency limit only: a linear-phase FIR
//      separates the low band exactly, and only the pop's span is attenuated.
//      The depth is Strength x how far that same band rises above its own local
//      level, so where the band is just the voice's low fundamental nothing is
//      cut. Outside pops the audio passes through bit for bit.
//
// Plain C++17, shared by the plugin, the standalone app and the CLI.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace pt
{
struct Params
{
    float sensitivity = 50.0f;  // 0..100
    float strength    = 80.0f;  // 0..100 %: how much of each pop's excess is removed
    float limitHz     = 100.0f; // 50..500: highest frequency reduced (100 keeps the voice's low notes intact)
    bool  popsOnly    = false;  // audition what is removed
    bool  bypass      = false;  // latency-compensated dry signal
};

class Engine
{
public:
    struct Pop
    {
        int64_t start = 0, end = 0; // reduced span, input-sample positions
        float excessDb = 0.0f;      // pop level above the local low end
        float reductionDb = 0.0f;   // applied reduction below the frequency limit
    };

    // Optional hook for offline tools (not used on the audio thread).
    std::function<void (const Pop&)> onPop;

    void prepare (double sampleRate, int maxChannels)
    {
        fs    = sampleRate;
        numCh = std::max (1, maxChannels);
        H     = std::max (1, (int) std::lround (fs * 0.0025));
        W     = 4 * H;
        LA    = hops (150.0);
        maxDur = hops (120.0);
        Kmax  = (int) std::ceil (5.5 * fs / 50.0);
        D     = (LA + maxDur + 4) * H + W + (int) (fs * 0.020) + Kmax;

        size = 1;
        while (size < D + Kmax + (int) fs)
            size <<= 1;
        in.assign ((size_t) numCh, std::vector<float> ((size_t) size, 0.0f));
        gain.assign ((size_t) size, 1.0f);
        sq.assign (4, std::vector<double> ((size_t) size, 0.0));
        envSize = 4096;
        env.assign (4, std::vector<float> ((size_t) envSize, -140.0f));
        medBuf.reserve ((size_t) (2 * LA + 1));
        fir.reserve ((size_t) (2 * Kmax + 1));
        designFilters();
        reset();
    }

    void reset()
    {
        for (auto& c : in)
            std::fill (c.begin(), c.end(), 0.0f);
        std::fill (gain.begin(), gain.end(), 1.0f);
        for (auto& s : sq)
            std::fill (s.begin(), s.end(), 0.0);
        for (auto& e : env)
            std::fill (e.begin(), e.end(), -140.0f);
        for (auto& b : bq)
            b.z1 = b.z2 = 0.0;
        sums[0] = sums[1] = sums[2] = sums[3] = 0.0;
        received = 0;
        hop = 0;
        inPop = false;
        popStart = 0;
        popPeak = popPeakExcess = popPeakLimit = 0.0f;
        speechDb = -20.0f;
        voiceRatio = -20.0f;
        total = 0;
        countOut.store (0);
        firLimit = -1.0f;
    }

    void setParams (const Params& p) { params = p; }
    int latencySamples() const { return D; }
    int64_t popCount() const { return total; }
    int64_t popCountShared() const { return countOut.load(); } // safe to read from the UI

    void process (float* const* ch, int nch, int ns)
    {
        nch = std::min (nch, numCh);
        if (nch <= 0)
            return;
        updateFir();

        for (int i = 0; i < ns; ++i)
        {
            const int w = idx (received);
            double m = 0.0;
            for (int c = 0; c < nch; ++c)
            {
                in[(size_t) c][(size_t) w] = ch[c][i];
                m += ch[c][i];
            }
            m /= nch;

            // Detection bands, the band below the limit, and their 10 ms running energies.
            const double lf = run (m, 0, 3), f0 = run (m, 3, 2), lim = run (m, 5, 2);
            const double e[4] = { lf * lf, f0 * f0, m * m, lim * lim };
            for (int b = 0; b < 4; ++b)
            {
                sums[b] += e[b] - sq[(size_t) b][(size_t) idx (received - W)];
                sq[(size_t) b][(size_t) w] = e[b];
            }
            ++received;
            if (received % H == 0)
                hopReady();

            const int64_t o = received - 1 - D;
            const int r = idx (o);
            const float g = gain[(size_t) r];
            for (int c = 0; c < nch; ++c)
            {
                const float x = in[(size_t) c][(size_t) r];
                float out = x;
                if (o < 0)
                    out = 0.0f;
                else if (! params.bypass && g < 1.0f)
                {
                    const float removed = (1.0f - g) * lowBand (in[(size_t) c], o);
                    out = params.popsOnly ? removed : x - removed;
                }
                else if (params.popsOnly && ! params.bypass)
                    out = 0.0f;
                ch[c][i] = out;
            }
            gain[(size_t) r] = 1.0f;
        }
        countOut.store (total);
    }

private:
    int idx (int64_t n) const { return (int) (n & (int64_t) (size - 1)); }
    int hops (double ms) const { return std::max (1, (int) std::lround (ms * 0.001 * fs / H)); }
    float& envAt (int b, int64_t k) { return env[(size_t) b][(size_t) (k & (envSize - 1))]; }

    // ---- detection filters: 20-80 Hz (HP2 + LP4) and 100-300 Hz (HP2 + LP2) ----
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; };
    Biquad bq[7];

    static Biquad design (double fc, double q, bool high, double fs)
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

    void designFilters()
    {
        bq[0] = design (20.0, 0.7071, true, fs);
        bq[1] = design (80.0, 0.5412, false, fs);
        bq[2] = design (80.0, 1.3066, false, fs);
        bq[3] = design (100.0, 0.7071, true, fs);
        bq[4] = design (300.0, 0.7071, false, fs);
    }

    double run (double x, int first, int count)
    {
        for (int s = first; s < first + count; ++s)
        {
            auto& f = bq[s];
            const double y = f.b0 * x + f.z1;
            f.z1 = f.b1 * x - f.a1 * y + f.z2;
            f.z2 = f.b2 * x - f.a2 * y;
            x = y;
        }
        return x;
    }

    // ---- per 2.5 ms hop: envelopes, then decisions LA hops back ----
    void hopReady()
    {
        const int64_t k = hop++;
        for (int b = 0; b < 4; ++b)
            envAt (b, k) = (float) (10.0 * std::log10 (std::max (0.0, sums[b]) / W + 1.0e-14));
        // Running level of loud speech (pops must be loud enough to matter), and
        // this voice's usual 20-80 Hz level relative to its fundamental band.
        if (envAt (2, k) > speechDb - 15.0f)
        {
            const float a = 1.0f - std::exp (-(float) H / (float) (fs * 2.0));
            speechDb += (envAt (2, k) - speechDb) * a;
            voiceRatio += (envAt (0, k) - envAt (1, k) - voiceRatio) * a;
        }
        if (k >= 2 * LA)
            decide (k - LA);
    }

    float median (int b, int64_t a, int64_t z)
    {
        medBuf.clear();
        for (int64_t j = a; j <= z; ++j)
            medBuf.push_back (envAt (b, j));
        std::nth_element (medBuf.begin(), medBuf.begin() + (std::ptrdiff_t) (medBuf.size() / 2), medBuf.end());
        return medBuf[medBuf.size() / 2];
    }

    void decide (int64_t d)
    {
        const float u = std::clamp (params.sensitivity / 100.0f, 0.0f, 1.0f);
        const float lf = envAt (0, d), f0 = envAt (1, d), full = envAt (2, d);
        const float local = median (0, d - LA, d + LA);
        const float excess = lf - local;
        // What the voice explains in this band: its local level, or its fundamental
        // at this moment shifted by the voice's usual ratio (+3 dB of slack).
        const float explained = 10.0f * std::log10 (std::pow (10.0f, local / 10.0f)
                                                    + std::pow (10.0f, (f0 + voiceRatio + 3.0f) / 10.0f));
        const float over = lf - explained;
        const bool cand = over > 14.0f - 10.0f * u && excess > 9.0f - 6.0f * u && full > -65.0f
                       && lf > speechDb - (25.0f + 20.0f * u); // too faint to hear: leave it

        if (cand)
        {
            const float excessLimit = envAt (3, d) - median (3, d - LA, d + LA);
            if (! inPop)
            {
                inPop = true;
                popStart = d;
                popPeak = over;
                popPeakExcess = excess;
                popPeakLimit = excessLimit;
            }
            popPeak = std::max (popPeak, over);
            popPeakExcess = std::max (popPeakExcess, excess);
            popPeakLimit = std::max (popPeakLimit, excessLimit);
            if (d - popStart < maxDur)
                return;
        }
        if (! inPop)
            return;
        inPop = false;
        if (d - popStart >= maxDur) // long low-end energy is not a pop
            return;

        Pop p;
        p.excessDb = popPeakExcess;
        // Cut the band below the limit only by how far the pop lifts that band,
        // and never by more than the pop rises above what the voice explains.
        p.reductionDb = std::clamp (params.strength / 100.0f, 0.0f, 1.0f)
                      * std::max (0.0f, std::min (popPeakLimit - 3.0f, popPeak));
        if (p.reductionDb < 0.5f)
            return;
        // A hop's energy covers the 10 ms ending at it; the band filters lag a little.
        p.start = std::max<int64_t> (0, (popStart + 1) * H - W - (int64_t) (fs * 0.020));
        p.end = d * H + (int64_t) (fs * 0.025);
        applyGain (p);
        ++total;
        if (onPop)
            onPop (p);
    }

    void applyGain (const Pop& p)
    {
        const float g = std::pow (10.0f, -p.reductionDb / 20.0f);
        const int64_t len = p.end - p.start;
        const int64_t up = std::min<int64_t> ((int64_t) (fs * 0.010), len / 3);
        const int64_t down = std::min<int64_t> ((int64_t) (fs * 0.015), len / 3);
        for (int64_t n = p.start; n < p.end; ++n)
        {
            float shape = 1.0f; // 1 = full reduction
            if (n - p.start < up)
                shape = 0.5f - 0.5f * std::cos (3.14159265f * (float) (n - p.start) / (float) up);
            else if (p.end - n < down)
                shape = 0.5f - 0.5f * std::cos (3.14159265f * (float) (p.end - n) / (float) down);
            const float v = 1.0f - shape * (1.0f - g);
            float& cur = gain[(size_t) idx (n)];
            cur = std::min (cur, v);
        }
    }

    // ---- exact low band at output time: linear-phase windowed-sinc low-pass ----
    void updateFir()
    {
        const float limit = std::clamp (params.limitHz, 50.0f, 500.0f);
        if (std::abs (limit - firLimit) < 0.5f)
            return;
        firLimit = limit;
        bq[5] = design (limit, 0.5412, false, fs); // energy of the band below the limit (sizes the cut)
        bq[6] = design (limit, 1.3066, false, fs);
        K = std::min (Kmax, (int) std::ceil (5.5 * fs / limit));
        fir.assign ((size_t) (2 * K + 1), 0.0f);
        double sum = 0.0;
        for (int n = -K; n <= K; ++n)
        {
            const double x = 6.283185307179586 * limit / fs;
            const double sinc = n == 0 ? x / 3.141592653589793 : std::sin (x * n) / (3.141592653589793 * n);
            const double t = (double) (n + K) / (2.0 * K);
            const double win = 0.42 - 0.5 * std::cos (6.283185307179586 * t) + 0.08 * std::cos (12.566370614359172 * t);
            fir[(size_t) (n + K)] = (float) (sinc * win);
            sum += sinc * win;
        }
        for (auto& v : fir)
            v = (float) (v / sum);
    }

    float lowBand (const std::vector<float>& x, int64_t n) const
    {
        double acc = 0.0;
        for (int k = -K; k <= K; ++k)
            acc += (double) fir[(size_t) (k + K)] * (double) x[(size_t) idx (n - k)];
        return (float) acc;
    }

    Params params;
    double fs = 44100.0;
    int numCh = 1, H = 110, W = 440, LA = 60, maxDur = 48, Kmax = 0, K = 0, D = 0, size = 0, envSize = 4096;
    int64_t received = 0, hop = 0, popStart = 0, total = 0;
    bool inPop = false;
    float popPeak = 0.0f, popPeakExcess = 0.0f, popPeakLimit = 0.0f, firLimit = -1.0f;
    float speechDb = -20.0f, voiceRatio = -20.0f;
    double sums[4] = {};
    std::vector<std::vector<float>> in, env;
    std::vector<std::vector<double>> sq;
    std::vector<float> gain, medBuf, fir;
    std::atomic<int64_t> countOut { 0 };
};
} // namespace pt
