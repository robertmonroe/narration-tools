#pragma once
// GlitchTamer DSP core: finds the clicks and pops that AI voice generators leave
// in pauses where they join audio chunks ("seams"), and repairs them with the
// room tone learned from the recording.
//
//   1. A 1 ms energy envelope on a 0.5 ms grid finds impulses: energy far above
//      the median of the surrounding 40 ms.
//   2. An impulse is a glitch only when it sits in a pause (quiet surroundings)
//      and no speech follows within the follow window. Real consonants ("t",
//      "k", "p") always run straight into speech, so they are left alone.
//   3. Static bursts: a short (<= 80 ms) rise well above the room tone with
//      silence on both sides, too short to be a word or a breath.
//   4. The repair covers the spike and its ringing tail (up to the repair
//      width), or the burst and its fade. A short STFT floors that span to the
//      learned room-tone spectrum, adding only the difference, so all other
//      audio stays bit-exact.
//
// Plain C++17, shared by the plugin, the standalone app and the CLI.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

namespace gt
{
struct Params
{
    float sensitivity  = 70.0f; // 0..100
    float widthMs      = 30.0f; // longest repair after the spike (covers its tail)
    bool  glitchesOnly = false; // audition what is removed
    bool  bypass       = false; // latency-compensated dry signal
};

class FFT
{
public:
    void init (int size)
    {
        n = size;
        int bits = 0;
        while ((1 << bits) < n)
            ++bits;
        rev.assign ((size_t) n, 0);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b))
                    r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        tw.resize ((size_t) n / 2);
        for (int k = 0; k < n / 2; ++k)
        {
            const double a = -6.283185307179586 * k / n;
            tw[(size_t) k] = { (float) std::cos (a), (float) std::sin (a) };
        }
    }

    void forward (std::complex<float>* a) const
    {
        for (int i = 0; i < n; ++i)
            if (i < rev[(size_t) i])
                std::swap (a[i], a[rev[(size_t) i]]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; ++j)
                {
                    const auto v = a[i + j + half] * tw[(size_t) (j * step)];
                    a[i + j + half] = a[i + j] - v;
                    a[i + j] += v;
                }
        }
    }

    void inverse (std::complex<float>* a) const
    {
        for (int i = 0; i < n; ++i)
            a[i] = std::conj (a[i]);
        forward (a);
        const float s = 1.0f / (float) n;
        for (int i = 0; i < n; ++i)
            a[i] = std::conj (a[i]) * s;
    }

private:
    int n = 0;
    std::vector<int> rev;
    std::vector<std::complex<float>> tw;
};

class Engine
{
public:
    struct Glitch
    {
        int64_t start = 0, end = 0, peak = 0; // input-sample positions
        float levelDb = 0.0f, jumpDb = 0.0f;  // peak level, and its rise above the surroundings
        bool burst = false;                   // static burst (otherwise a click)
    };

    // Optional hook for offline tools (not used on the audio thread).
    std::function<void (const Glitch&)> onGlitch;

    void prepare (double sampleRate, int maxChannels)
    {
        fs    = sampleRate;
        numCh = std::max (1, maxChannels);
        h     = std::max (1, (int) std::lround (fs * 0.0005));
        w     = 2 * h;
        LA    = steps (190.0f);
        D     = (LA + steps (110.0f)) * h + w; // bursts are decided at their end, up to ~100 ms after they start
        subLen = steps (500.0f);

        anaSize = nextPow2 (w + 1);
        ana.assign ((size_t) anaSize, 0.0f);
        e1.assign (kRing, 0.0f);
        lv.assign (kRing, -140.0f);
        lev10.assign (kRing, -140.0f);
        floorAt.assign (kRing, -80.0f);
        speechAt.assign (kRing, -20.0f);
        medBuf.resize (81);

        delSize = nextPow2 (D + 1);
        del.assign ((size_t) numCh, std::vector<float> ((size_t) delSize, 0.0f));

        S  = fs <= 50000.0 ? 512 : fs <= 100000.0 ? 1024 : 2048;
        Hs = S / 4;
        sfft.init (S);
        sw.resize ((size_t) S);
        for (int i = 0; i < S; ++i)
            sw[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (6.283185307179586 * i / S));
        double olaSum = 0.0;
        for (int i = 0; i < S; i += Hs)
            olaSum += (double) sw[(size_t) i] * sw[(size_t) i];
        olaNorm = (float) (1.0 / olaSum);

        synSize = nextPow2 (2 * S + 1);
        const size_t bins = (size_t) S / 2 + 1;
        syn.assign ((size_t) numCh, std::vector<float> ((size_t) synSize, 0.0f));
        ola.assign ((size_t) numCh, std::vector<float> ((size_t) synSize, 0.0f));
        amb.assign ((size_t) numCh, std::vector<float> (bins, 0.0f));
        ambCount.assign ((size_t) numCh, 0);
        sbuf.assign ((size_t) S, {});
        gbin.assign (bins, 1.0f);
        gsm.assign (bins, 1.0f);

        reset();
    }

    void reset()
    {
        std::fill (ana.begin(), ana.end(), 0.0f);
        std::fill (e1.begin(), e1.end(), 0.0f);
        std::fill (lv.begin(), lv.end(), -140.0f);
        std::fill (lev10.begin(), lev10.end(), -140.0f);
        std::fill (floorAt.begin(), floorAt.end(), -80.0f);
        std::fill (speechAt.begin(), speechAt.end(), -20.0f);
        for (auto* rings : { &del, &syn, &ola, &amb })
            for (auto& r : *rings)
                std::fill (r.begin(), r.end(), 0.0f);
        std::fill (ambCount.begin(), ambCount.end(), 0);
        anaPos = delPos = hopCount = 0;
        received = fine = 0;
        sum10 = 0.0;
        subMin = std::numeric_limits<float>::infinity();
        subCount = subIdx = 0;
        std::fill (std::begin (subMins), std::end (subMins), std::numeric_limits<float>::infinity());
        floorDb = -80.0f;
        speechDb = -20.0f;
        lastPeak = -1000000;
        inBurst = false;
        burstStart = 0;
        regions.clear();
        synCount = 0;
        synHop = 0;
        total = 0;
        countOut.store (0);
    }

    void setParams (const Params& p) { params = p; }
    int latencySamples() const { return D + S; }
    int64_t glitchCount() const { return total; }
    int64_t glitchCountShared() const { return countOut.load(); } // safe to read from the UI
    double sampleRate() const { return fs; }

    void process (float* const* ch, int nch, int ns)
    {
        nch = std::min (nch, numCh);
        if (nch <= 0)
            return;

        for (int i = 0; i < ns; ++i)
        {
            float mono = 0.0f;
            for (int c = 0; c < nch; ++c)
                mono += ch[c][i];
            ana[(size_t) anaPos] = mono / (float) nch;
            anaPos = (anaPos + 1) & (anaSize - 1);
            for (int c = 0; c < nch; ++c)
                del[(size_t) c][(size_t) delPos] = ch[c][i];

            ++received;
            if (++hopCount == h)
            {
                hopCount = 0;
                analyseStep();
            }

            // Detection-delayed sample into the synthesis stage.
            const int rp = (delPos - D) & (delSize - 1);
            const int sp = (int) (synCount & (synSize - 1));
            for (int c = 0; c < nch; ++c)
                syn[(size_t) c][(size_t) sp] = del[(size_t) c][(size_t) rp];
            delPos = (delPos + 1) & (delSize - 1);

            ++synCount;
            if (++synHop == Hs)
            {
                synHop = 0;
                synthFrame (nch);
            }

            const int64_t v = synCount - 1 - S;
            const int op = (int) (v & (synSize - 1));
            for (int c = 0; c < nch; ++c)
            {
                auto& o = ola[(size_t) c][(size_t) op];
                const float x = syn[(size_t) c][(size_t) op];
                ch[c][i] = v < 0 ? 0.0f : params.bypass ? x : params.glitchesOnly ? -o : x + o;
                o = 0.0f;
            }
        }
        countOut.store (total);
    }

private:
    static constexpr size_t kRing = 4096; // fine steps of history (~2 s)

    static int nextPow2 (int v) { int p = 1; while (p < v) p <<= 1; return p; }
    int steps (float ms) const { return std::max (1, (int) std::ceil (ms * 0.001 * fs / h)); }
    static size_t ri (int64_t k) { return (size_t) (k & (int64_t) (kRing - 1)); }
    int64_t centre (int64_t k) const { return (k + 1) * h - w / 2; } // input position of fine step k
    int64_t stepAt (int64_t pos) const { return (pos + w / 2) / h - 1; }

    struct Thresholds { float jumpDb, followSteps, burstRiseDb; };

    Thresholds thresholds() const
    {
        const float u = std::clamp (params.sensitivity / 100.0f, 0.0f, 1.0f);
        return { 26.0f - 12.0f * u,                    // spike rise above its surroundings
                 (float) steps (150.0f - 90.0f * u),   // pause that must follow the spike
                 20.0f - 8.0f * u };                   // static burst rise above the room tone
    }

    void analyseStep()
    {
        const int64_t f = fine++;

        double e = 0.0;
        for (int i = 0; i < w; ++i)
        {
            const float x = ana[(size_t) ((anaPos - w + i) & (anaSize - 1))];
            e += (double) x * x;
        }
        e /= w;
        sum10 += e - e1[ri (f - 20)];
        e1[ri (f)] = (float) e;
        lv[ri (f)] = (float) (10.0 * std::log10 (e + 1.0e-14));
        const float l10 = (float) (10.0 * std::log10 (std::max (0.0, sum10) / 20.0 + 1.0e-14));
        lev10[ri (f)] = l10;

        // Room-tone floor: minimum statistics of 10 ms levels over ~4 s.
        if (f > 40 && l10 > -100.0f)
            subMin = std::min (subMin, l10);
        if (++subCount >= subLen)
        {
            subMins[subIdx++ % kSubs] = subMin;
            subMin = std::numeric_limits<float>::infinity();
            subCount = 0;
        }
        float nf = subMin;
        for (float v : subMins)
            nf = std::min (nf, v);
        floorDb = std::isfinite (nf) ? nf : -80.0f;

        // Typical loud speech level.
        if (l10 > floorDb + 30.0f)
            speechDb += (l10 - speechDb) * (1.0f - std::exp (-(float) h / (float) (fs * 2.0)));

        floorAt[ri (f)] = floorDb;
        speechAt[ri (f)] = speechDb;

        const int64_t d = f - LA;
        if (d >= 40)
        {
            decide (d);
            decideBurst (d);
        }
    }

    // Static bursts, tracked on the 10 ms level: a short rise well above the room
    // tone, judged when it ends, with quiet before it and after its fade.
    void decideBurst (int64_t d)
    {
        const auto th = thresholds();
        const bool above = lev10[ri (d)] > floorAt[ri (d)] + th.burstRiseDb;
        if (above && ! inBurst)
        {
            inBurst = true;
            burstStart = d;
            return;
        }
        if (above || ! inBurst)
            return;

        inBurst = false;
        const int64_t s = burstStart, e = d; // lev10 above the threshold on [s, e)
        if (e - s > steps (80.0f))
            return;

        const float quiet = floorAt[ri (d)] + 8.0f;
        auto maxLev = [this] (int64_t a, int64_t b)
        {
            float m = -140.0f;
            for (int64_t k = a; k <= b; ++k)
                m = std::max (m, lev10[ri (k)]);
            return m;
        };
        const float peak = maxLev (s, e - 1);
        if (maxLev (s - steps (80.0f), s - steps (10.0f)) >= quiet
            || maxLev (e + steps (30.0f), e + steps (100.0f)) >= quiet
            || peak >= speechAt[ri (d)] - 15.0f)
            return;

        // lev10[k] averages the 10 ms ending at step k: the burst starts ~10 ms
        // before s and fades out within ~30 ms after e.
        Glitch g;
        g.start = std::max<int64_t> (0, (s + 1) * h - steps (15.0f) * h);
        g.end = (e + 1) * h + steps (30.0f) * h;
        g.peak = (s + e) / 2 * h;
        g.levelDb = peak;
        g.jumpDb = peak - floorAt[ri (d)];
        g.burst = true;

        // A click's own fizz can also look like a burst: widen that repair, count it once.
        const bool partOfClick = std::any_of (regions.begin(), regions.end(),
                                              [&] (const Glitch& r) { return r.start <= g.end && g.start <= r.end; });
        insertRegion (g);
        if (partOfClick)
            return;
        ++total;
        if (onGlitch)
            onGlitch (g);
    }

    // Keeps repair regions sorted by start and merges overlaps.
    void insertRegion (const Glitch& g)
    {
        auto it = std::find_if (regions.begin(), regions.end(), [&] (const Glitch& r) { return r.start > g.start; });
        it = regions.insert (it, g);
        if (it != regions.begin() && std::prev (it)->end >= it->start)
        {
            std::prev (it)->end = std::max (std::prev (it)->end, it->end);
            it = std::prev (regions.erase (it));
        }
        while (std::next (it) != regions.end() && std::next (it)->start <= it->end)
        {
            it->end = std::max (it->end, std::next (it)->end);
            regions.erase (std::next (it));
        }
    }

    void decide (int64_t d)
    {
        const auto th = thresholds();
        const float peak = lv[ri (d)];
        if (peak <= -65.0f || peak <= floorAt[ri (d)] + 15.0f || d - lastPeak < 20)
            return;

        // Must be the first maximum within +-5 ms.
        for (int64_t j = d - 10; j <= d + 10; ++j)
            if (lv[ri (j)] > peak || (j < d && lv[ri (j)] == peak))
                return;

        const float med = median (d - 40, d + 40);
        const float speech = speechAt[ri (d)];
        if (peak - med < th.jumpDb || med > speech - 22.0f)
            return;

        // A real consonant runs into speech; a seam glitch is followed by a pause.
        float after = -140.0f;
        for (int64_t j = d + 70; j <= d + 50 + (int64_t) th.followSteps + 20; ++j)
            after = std::max (after, lev10[ri (j)]);
        if (after > speech - 18.0f)
            return;

        int back = 4;
        while (back < 10 && lv[ri (d - back - 1)] > med + 6.0f)
            ++back;
        const float post = median (d + 60, d + 100);
        const int maxFwd = std::max (10, steps (params.widthMs));
        int fwd = 10;
        while (fwd < maxFwd && lv[ri (d + fwd + 1)] > post + 6.0f)
            ++fwd;

        Glitch g;
        g.start = std::max<int64_t> (0, centre (d - back) - w);
        g.end = centre (d + fwd) + w;
        g.peak = centre (d);
        g.levelDb = peak;
        g.jumpDb = peak - med;
        lastPeak = d + fwd;

        insertRegion (g);
        ++total;
        if (onGlitch)
            onGlitch (g);
    }

    float median (int64_t a, int64_t b)
    {
        const size_t n = (size_t) (b - a + 1);
        if (medBuf.size() < n)
            medBuf.resize (n);
        for (size_t i = 0; i < n; ++i)
            medBuf[i] = lv[ri (a + (int64_t) i)];
        std::nth_element (medBuf.begin(), medBuf.begin() + (std::ptrdiff_t) (n / 2), medBuf.begin() + (std::ptrdiff_t) n);
        return medBuf[n / 2];
    }

    // Repair amount for a synthesis frame over input positions [a, a + S):
    // the window weight of the closest repaired sample (1 when it covers the centre).
    float frameMask (int64_t a)
    {
        while (! regions.empty() && regions.front().end < a - 2 * S)
            regions.pop_front();
        const int64_t c = a + S / 2;
        float m = 0.0f;
        for (const auto& r : regions)
        {
            if (r.start >= a + S)
                break;
            if (r.end <= a)
                continue;
            const int64_t s = std::max (r.start, a), e = std::min (r.end, a + S);
            const int64_t nearest = c < s ? s : c >= e ? e - 1 : c;
            const double x = (double) (nearest - a) / S;
            m = std::max (m, (float) (0.5 - 0.5 * std::cos (6.283185307179586 * x)));
        }
        return m;
    }

    bool isRoomTone (int64_t pos) const
    {
        const int64_t k = stepAt (pos) + 10; // lev10 at k covers the 10 ms ending there
        if (k < 40 || k > fine - 1)
            return false;
        // Well below speech too: right after playback starts mid-speech the floor
        // estimate has only seen speech, and quiet syllables must not count as room tone.
        const float l = lev10[ri (k)];
        return l > -100.0f && l < floorAt[ri (k)] + 4.0f && l < speechAt[ri (k)] - 30.0f;
    }

    void synthFrame (int nch)
    {
        const int64_t start = synCount - S; // synthesis index; input position = index - D
        if (start < 0)
            return;
        const int64_t a = start - D;
        const float m = frameMask (a);
        const bool learn = m <= 0.0f && isRoomTone (a + S / 2);
        if (m <= 0.0f && ! learn)
            return;

        const int half = S / 2;
        for (int c = 0; c < nch; ++c)
        {
            auto& am = amb[(size_t) c];
            for (int n = 0; n < S; ++n)
                sbuf[(size_t) n] = { syn[(size_t) c][(size_t) ((start + n) & (synSize - 1))] * sw[(size_t) n], 0.0f };
            sfft.forward (sbuf.data());

            if (learn)
            {
                for (int b = 0; b <= half; ++b)
                {
                    const float p = std::norm (sbuf[(size_t) b]);
                    am[(size_t) b] = ambCount[(size_t) c] == 0 ? p : am[(size_t) b] + 0.05f * (p - am[(size_t) b]);
                }
                if (ambCount[(size_t) c] < 1000000)
                    ++ambCount[(size_t) c];
                continue;
            }

            // Floor every bin to the room tone (or cut 30 dB before any is learned).
            const bool haveAmb = ambCount[(size_t) c] > 0;
            for (int b = 0; b <= half; ++b)
            {
                const float p = std::max (std::norm (sbuf[(size_t) b]), 1.0e-20f);
                gbin[(size_t) b] = haveAmb ? std::sqrt (std::min (1.0f, am[(size_t) b] / p)) : 0.03f;
            }
            for (int b = 0; b <= half; ++b)
                gsm[(size_t) b] = 0.25f * gbin[(size_t) std::max (0, b - 1)] + 0.5f * gbin[(size_t) b]
                                + 0.25f * gbin[(size_t) std::min (half, b + 1)];
            for (int b = 0; b <= half; ++b)
            {
                const float dlt = -m * (1.0f - gsm[(size_t) b]); // gain - 1: the difference signal
                sbuf[(size_t) b] *= dlt;
                if (b > 0 && b < half)
                    sbuf[(size_t) (S - b)] *= dlt;
            }
            sfft.inverse (sbuf.data());
            for (int n = 0; n < S; ++n)
                ola[(size_t) c][(size_t) ((start + n) & (synSize - 1))] += sbuf[(size_t) n].real() * sw[(size_t) n] * olaNorm;
        }
    }

    Params params;
    double fs = 44100.0;
    int numCh = 1, h = 22, w = 44, LA = 380, D = 0, subLen = 1000;
    int anaSize = 0, anaPos = 0, delSize = 0, delPos = 0, hopCount = 0;
    int64_t received = 0, fine = 0, lastPeak = -1000000, total = 0;
    double sum10 = 0.0;
    std::vector<float> ana, e1, lv, lev10, floorAt, speechAt, medBuf;
    std::vector<std::vector<float>> del;

    static constexpr int kSubs = 8;
    int subCount = 0, subIdx = 0;
    float subMin = 0.0f, subMins[kSubs] {};
    float floorDb = -80.0f, speechDb = -20.0f;
    bool inBurst = false;
    int64_t burstStart = 0;
    std::deque<Glitch> regions;

    FFT sfft;
    int S = 512, Hs = 128, synSize = 0, synHop = 0;
    int64_t synCount = 0;
    float olaNorm = 0.5f;
    std::vector<float> sw, gbin, gsm;
    std::vector<std::vector<float>> syn, ola, amb;
    std::vector<int> ambCount;
    std::vector<std::complex<float>> sbuf;
    std::atomic<int64_t> countOut { 0 };
};
} // namespace gt
