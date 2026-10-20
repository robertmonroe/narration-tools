#pragma once
// BreathTamer DSP core.
//
// Clean-room design built from the documented behaviour of a breath-control
// module ("detects breaths by their harmonic structure, not by level"):
//
//   1. Analyse ~46 ms frames every ~5.8 ms: level, periodicity (Boersma
//      normalised autocorrelation), spectral centroid and high-band share.
//   2. A frame is a breath *candidate* when it is unvoiced, non-sibilant, above
//      the running noise floor and below the running speech level.
//   3. Candidate runs (small gaps bridged) longer than a sensitivity-dependent
//      minimum duration become breaths. Lookahead makes this causal-free.
//   4. Breaths are trimmed to keep ~18 ms clear of neighbouring speech (voice,
//      sibilants, loud consonants), widened only into quiet room tone, smoothed
//      into ramps, and applied as Gain (fixed cut) or Target (cut to a level).
//   5. Natural mode reduces breaths spectrally, never below the learned room-tone
//      spectrum, so ambience continues under removed breaths. Gated mode (and the
//      Classic algorithm, which is BreathTamer 1.0) applies a broadband gain.
//   6. The Offline algorithm uses whole-file noise-floor and speech-level
//      statistics (see StatsCollector) instead of running estimates.
//
// Plain C++17 with no framework dependencies so the plugin, the standalone app
// and the command-line tool share exactly the same processing.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace bt
{
struct Params
{
    enum Mode { Gain = 0, Target = 1 };
    enum Algorithm { RealTime = 0, Offline = 1, Classic = 2 };

    int   mode        = Gain;
    float levelDb     = -20.0f;   // Gain: reduction amount; Target: breath level. -100 = -inf
    float sensitivity = 60.0f;    // 0..100
    int   algorithm   = RealTime;
    bool  natural     = true;     // keep room tone under breaths (not in Classic)
    bool  strictRoomTone = false; // Natural: learn room tone only from audio 30 dB below speech
    bool  breathsOnly = false;    // audition the detected breaths
    bool  bypass      = false;    // latency-compensated dry signal
};

// Whole-file statistics for the Offline algorithm.
struct Stats
{
    bool  valid    = false;
    float floorDb  = -80.0f;
    float speechDb = -24.0f;
};

inline float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }
inline float levelToGain (float db) { return db <= -99.95f ? 0.0f : dbToGain (db); }

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
    struct Frame
    {
        float levelDb = -200.0f, rms = 0.0f, harm = 0.0f, centroid = 0.0f, hfRatio = 0.0f;
        float floorDb = -80.0f, speechDb = -24.0f;
        bool  cand = false, blocked = false, breath = false;
        bool  guard = false;   // audible non-breath sound (voice, sibilant, loud consonant)
        bool  protect = false; // within the guard distance of a guard frame
        float wMask = 0.0f, wEnv = 1.0f; // widened
        float mask = 0.0f, env = 1.0f;   // smoothed, final
    };

    struct Region { int64_t start, end; }; // input-sample positions

    // Optional hooks for offline tools (not used on the audio thread).
    std::function<void (int64_t frame, const Frame&)> onDecision;
    std::function<void (const Region&)> onRegion;

    void prepare (double sampleRate, int maxChannels)
    {
        fs = sampleRate;
        N  = sampleRate <= 50000.0 ? 2048 : sampleRate <= 100000.0 ? 4096 : 8192;
        H  = N / 8;
        M  = 2 * N;
        fft.init (M);

        window.resize ((size_t) N);
        winPow = 0.0;
        for (int i = 0; i < N; ++i)
        {
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (6.283185307179586 * (i + 0.5) / N));
            winPow += (double) window[(size_t) i] * window[(size_t) i];
        }

        buf.assign ((size_t) M, {});
        for (int i = 0; i < N; ++i)
            buf[(size_t) i] = { window[(size_t) i], 0.0f };
        fft.forward (buf.data());
        for (auto& c : buf)
            c = { std::norm (c), 0.0f };
        fft.forward (buf.data());
        winAc.resize ((size_t) N);
        for (int t = 0; t < N; ++t)
            winAc[(size_t) t] = buf[(size_t) t].real() / buf[0].real();

        binHz   = fs / M;
        tauMin  = std::max (2, (int) (fs / 500.0));
        tauMax  = std::min (N / 2, (int) std::ceil (fs / 70.0));

        Fscan   = msToFrames (300.0f);
        G       = msToFrames (18.0f);
        Rw      = msToFrames (25.0f);
        R       = msToFrames (15.0f);
        P       = msToFrames (18.0f);
        lag     = Fscan + G + Rw + R;
        D       = (lag + 2) * H + N / 2;
        subLen  = msToFrames (600.0f);

        anaSize = nextPow2 (N);
        delSize = nextPow2 (D + 1);
        numCh   = std::max (1, maxChannels);
        ana.assign ((size_t) anaSize, 0.0f);
        del.assign ((size_t) numCh, std::vector<float> ((size_t) delSize, 0.0f));
        frames.assign (kRing, Frame{});
        frameData.assign ((size_t) N, 0.0f);

        // Synthesis STFT for Natural mode: sqrt-Hann analysis/synthesis, 75% overlap.
        S  = N / 2;
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
        synMask.assign ((size_t) synSize, 0.0f);
        synTgt.assign ((size_t) synSize, 1.0f);
        synGain.assign ((size_t) synSize, 1.0f);
        amb.assign ((size_t) numCh, std::vector<float> (bins, 0.0f));
        pwr.assign ((size_t) numCh, std::vector<float> (bins, 0.0f));
        ambCount.assign ((size_t) numCh, 0);
        sbuf.assign ((size_t) S, {});
        gbin.assign (bins, 1.0f);
        gsm.assign (bins, 1.0f);

        reset();
    }

    void reset()
    {
        std::fill (ana.begin(), ana.end(), 0.0f);
        for (auto& d : del)
            std::fill (d.begin(), d.end(), 0.0f);
        std::fill (frames.begin(), frames.end(), Frame{});
        anaPos = delPos = 0;
        hopCount = 0;
        received = 0;
        frameCount = 0;
        subMin = std::numeric_limits<float>::infinity();
        subCount = 0;
        subIdx = 0;
        std::fill (std::begin (subMins), std::end (subMins), std::numeric_limits<float>::infinity());
        noiseFloorDb = -80.0f;
        speechDb = -24.0f;
        inBreath = false;
        segStart = 0;
        breathTotal = 0;
        meter.store (0.0f);

        for (auto* rings : { &syn, &ola, &amb, &pwr })
            for (auto& r : *rings)
                std::fill (r.begin(), r.end(), 0.0f);
        std::fill (synMask.begin(), synMask.end(), 0.0f);
        std::fill (synTgt.begin(), synTgt.end(), 1.0f);
        std::fill (synGain.begin(), synGain.end(), 1.0f);
        std::fill (ambCount.begin(), ambCount.end(), 0);
        synCount = 0;
        synHop = 0;
        synActive = false;
    }

    void setParams (const Params& p) { params = p; }
    void setStats (const Stats& s) { stats = s; }
    int latencySamples() const { return D + S; }
    int64_t breathCount() const { return breathTotal; }
    float meterDb() const { return meter.load(); }
    double sampleRate() const { return fs; }

    void process (float* const* ch, int nch, int ns)
    {
        nch = std::min (nch, numCh);
        if (nch <= 0)
            return;

        const float levelLin = levelToGain (params.levelDb);
        const bool natural = params.natural && params.algorithm != Params::Classic
                          && ! params.bypass && ! params.breathsOnly;
        float minGain = 1.0f;

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
            if (++hopCount == H)
            {
                hopCount = 0;
                analyseFrame();
            }

            float mask = 0.0f, env = 1.0f;
            gainAt (received - 1 - D, mask, env);

            const float tgt = params.mode == Params::Target ? std::min (1.0f, levelLin / std::max (env, 1.0e-9f))
                                                            : levelLin;
            float g;
            if (params.bypass)
                g = 1.0f;
            else if (params.breathsOnly)
                g = mask;
            else
            {
                g = 1.0f - mask * (1.0f - tgt);
                minGain = std::min (minGain, g);
            }

            // Hand the detection-delayed sample to the synthesis stage.
            const int rp = (delPos - D) & (delSize - 1);
            const int sp = (int) (synCount & (synSize - 1));
            for (int c = 0; c < nch; ++c)
                syn[(size_t) c][(size_t) sp] = del[(size_t) c][(size_t) rp];
            synMask[(size_t) sp] = mask;
            synTgt[(size_t) sp]  = tgt;
            synGain[(size_t) sp] = g;
            delPos = (delPos + 1) & (delSize - 1);

            ++synCount;
            if (++synHop == Hs)
            {
                synHop = 0;
                synthFrame (nch, natural);
            }

            // Output S samples behind: every STFT frame touching it has been added.
            // Natural adds only the (output - input) difference, so untouched audio
            // stays bit-exact; Gated/Classic apply the broadband gain.
            const int64_t v = synCount - 1 - S;
            const int op = (int) (v & (synSize - 1));
            for (int c = 0; c < nch; ++c)
            {
                auto& o = ola[(size_t) c][(size_t) op];
                const float x = syn[(size_t) c][(size_t) op];
                ch[c][i] = v < 0 ? 0.0f : natural ? x + o : x * synGain[(size_t) op];
                o = 0.0f;
            }
        }

        meter.store (20.0f * std::log10 (std::max (minGain, 1.0e-5f)));
    }

private:
    static constexpr size_t kRing = 1024; // frames of history (> lag + scan range)

    static int nextPow2 (int v) { int p = 1; while (p < v) p <<= 1; return p; }
    int msToFrames (float ms) const { return std::max (1, (int) std::ceil (ms * 0.001 * fs / H)); }
    Frame& slot (int64_t k) { return frames[(size_t) (k & (int64_t) (kRing - 1))]; }
    int64_t centre (int64_t k) const { return (k + 1) * H - N / 2; }

    struct Thresholds { float harmMax, floorMargin, relMargin, hfMax, centroidMax; int minFrames; };

    Thresholds thresholds() const
    {
        const float u = std::clamp (params.sensitivity / 100.0f, 0.0f, 1.0f);
        // Spectral limits sit well above measured narration breaths (centroid ~1.8-2.5 kHz,
        // ~3% of energy above 5 kHz) and below sibilants (centroid ~5-6 kHz, >50% above 5 kHz).
        return { 0.30f + 0.30f * u,             // max periodicity of a breath frame
                 9.0f - 6.0f * u,               // dB above the noise floor
                 14.0f - 12.0f * u,             // dB below the running speech level
                 0.12f + 0.28f * u,             // max energy share above 5 kHz (sibilance guard)
                 3000.0f + 1500.0f * u,         // max spectral centroid, Hz
                 msToFrames (280.0f - 180.0f * u) };
    }

    void analyseFrame()
    {
        const int64_t k = frameCount++;
        Frame& f = slot (k);
        f = Frame{};

        double mean = 0.0;
        for (int i = 0; i < N; ++i)
        {
            frameData[(size_t) i] = ana[(size_t) ((anaPos - N + i) & (anaSize - 1))];
            mean += frameData[(size_t) i];
        }
        mean /= N;

        double energy = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const float v = (float) (frameData[(size_t) i] - mean) * window[(size_t) i];
            buf[(size_t) i] = { v, 0.0f };
            energy += (double) v * v;
        }
        std::fill (buf.begin() + N, buf.end(), std::complex<float>{});

        const double power = energy / winPow;
        f.levelDb = (float) (10.0 * std::log10 (power + 1.0e-20));
        f.rms = (float) std::sqrt (power);

        fft.forward (buf.data());

        // Spectral shape over 150 Hz .. 12 kHz; harmonicity over 60 Hz .. 5 kHz.
        const int half = M / 2;
        double tot = 0.0, hi = 0.0, cen = 0.0;
        for (int b = 0; b < M; ++b)
        {
            const float p = std::norm (buf[(size_t) b]);
            const double hz = (b <= half ? b : M - b) * binHz;
            if (b <= half && hz >= 150.0 && hz <= 12000.0)
            {
                tot += p;
                cen += p * hz;
                if (hz >= 5000.0)
                    hi += p;
            }
            buf[(size_t) b] = { (hz >= 60.0 && hz <= 5000.0) ? p : 0.0f, 0.0f };
        }
        f.centroid = tot > 0.0 ? (float) (cen / tot) : 0.0f;
        f.hfRatio  = tot > 0.0 ? (float) (hi / tot) : 0.0f;

        fft.forward (buf.data()); // power spectrum -> autocorrelation (real, symmetric)
        const float r0 = buf[0].real();
        float harm = 0.0f;
        if (r0 > 0.0f && f.levelDb > -90.0f)
            for (int t = tauMin; t <= tauMax; ++t)
                if (winAc[(size_t) t] > 0.05f)
                    harm = std::max (harm, buf[(size_t) t].real() / r0 / winAc[(size_t) t]);
        f.harm = std::min (harm, 1.0f);

        // Noise floor: minimum statistics over ~4.8 s, ignoring digital silence and
        // the first frames, which still contain the analysis buffer's zero fill.
        if (f.levelDb > -100.0f && k * H >= N)
            subMin = std::min (subMin, f.levelDb);
        if (++subCount >= subLen)
        {
            subMins[subIdx++ % kSubs] = subMin;
            subMin = std::numeric_limits<float>::infinity();
            subCount = 0;
        }
        float nf = subMin;
        for (float v : subMins)
            nf = std::min (nf, v);
        noiseFloorDb = std::isfinite (nf) ? nf : -80.0f;

        // Running level of clearly voiced speech.
        if (f.harm > 0.75f && f.levelDb > noiseFloorDb + 20.0f)
        {
            const float a = 1.0f - std::exp (-(float) H / (float) (fs * 1.5));
            speechDb += (f.levelDb - speechDb) * a;
        }

        // Offline replaces the running estimates with whole-file statistics.
        const bool whole = params.algorithm == Params::Offline && stats.valid;
        f.floorDb  = whole ? stats.floorDb : noiseFloorDb;
        f.speechDb = whole ? stats.speechDb : speechDb;

        const auto th = thresholds();
        f.blocked = f.harm >= th.harmMax;
        f.cand = ! f.blocked
              && f.levelDb > -75.0f
              && f.levelDb > f.floorDb + th.floorMargin
              && f.levelDb < f.speechDb - th.relMargin
              && f.centroid < th.centroidMax
              && f.hfRatio < th.hfMax;

        // Audible sounds rejected for being voiced, too loud or too bright are speech;
        // breath attenuation must keep clear of them so onsets and word endings survive.
        f.guard = f.blocked
               || (! f.cand && f.levelDb > f.floorDb + th.floorMargin
                   && (f.levelDb >= f.speechDb - th.relMargin || f.centroid >= th.centroidMax || f.hfRatio >= th.hfMax));

        const int64_t j1 = k - (Fscan + G);
        if (j1 >= 0) decide (j1, th);
        const int64_t j2 = j1 - Rw;
        if (j2 >= 0) widen (j2);
        const int64_t j3 = j2 - R;
        if (j3 >= 0) smooth (j3);
    }

    bool closed (int64_t i)
    {
        if (i < 0)
            return false;
        const Frame& f = slot (i);
        if (f.cand)
            return true;
        if (f.blocked)
            return false;
        bool before = false, after = false;
        for (int d = 1; d <= G; ++d)
        {
            if (i - d >= 0 && slot (i - d).cand) before = true;
            if (slot (i + d).cand) after = true;
        }
        return before && after;
    }

    void decide (int64_t j, const Thresholds& th)
    {
        bool isBreath = false;
        if (closed (j))
        {
            int run = 1;
            for (int64_t i = j - 1; i >= 0 && run < th.minFrames && closed (i); --i) ++run;
            for (int64_t i = j + 1; i <= j + Fscan && run < th.minFrames && closed (i); ++i) ++run;
            isBreath = run >= th.minFrames;
        }

        Frame& f = slot (j);
        for (int64_t i = j - P; i <= j + P && ! f.protect; ++i)
            f.protect = i >= 0 && slot (i).guard;

        isBreath = isBreath && ! f.protect;
        f.breath = isBreath;

        if (isBreath && ! inBreath)
        {
            inBreath = true;
            segStart = j;
            ++breathTotal;
        }
        else if (! isBreath && inBreath)
        {
            inBreath = false;
            if (onRegion)
                onRegion ({ std::max<int64_t> (0, centre (segStart) - H / 2), centre (j - 1) + H / 2 });
        }

        if (onDecision)
            onDecision (j, f);
    }

    void widen (int64_t j)
    {
        Frame& f = slot (j);
        float wm = f.breath ? 1.0f : 0.0f, we = f.rms;
        for (int64_t i = j - Rw; i <= j + Rw; ++i)
        {
            if (i < 0) continue;
            const Frame& o = slot (i);
            if (! f.protect && o.breath) wm = 1.0f; // only spread into quiet room tone
            we = std::max (we, o.rms);
        }
        f.wMask = wm;
        f.wEnv = we;
    }

    void smooth (int64_t j)
    {
        float m = 0.0f, e = 0.0f;
        for (int64_t i = j - R; i <= j + R; ++i)
        {
            const Frame& o = slot (i); // negative indices hold defaults (mask 0)
            m += i < 0 ? 0.0f : o.wMask;
            e += i < 0 ? 1.0f : o.wEnv;
        }
        Frame& f = slot (j);
        f.mask = m / (float) (2 * R + 1);
        f.env  = e / (float) (2 * R + 1);
    }

    void gainAt (int64_t u, float& mask, float& env)
    {
        if (u < 0)
            return;
        const double p = (double) (u - H + N / 2) / H;
        const int64_t k0 = (int64_t) std::floor (p);
        const float fr = (float) (p - (double) k0);
        if (k0 + 1 < 0)
            return;
        const Frame& a = slot (k0);
        const Frame& b = slot (k0 + 1);
        const float ma = k0 < 0 ? 0.0f : a.mask, ea = k0 < 0 ? b.env : a.env;
        mask = ma + (b.mask - ma) * fr;
        env  = ea + (b.env - ea) * fr;
    }

    // True when the analysis frame nearest to input position `pos` is plain room tone.
    bool isRoomTone (int64_t pos)
    {
        const int64_t k = (int64_t) std::llround ((double) (pos - H + N / 2) / H);
        if (k < 0)
            return false;
        const Frame& f = slot (k);
        // Strict: also well below speech, so quiet syllables heard right after playback
        // starts mid-speech can't be mistaken for room tone.
        return ! f.breath && ! f.guard && ! f.cand && f.levelDb > -100.0f && f.levelDb < f.floorDb + 4.0f
            && (! params.strictRoomTone || f.levelDb < f.speechDb - 30.0f);
    }

    // One synthesis hop: learn the room-tone spectrum from ambience frames and, in
    // Natural mode, reduce breath frames per bin but never below that spectrum.
    void synthFrame (int nch, bool natural)
    {
        const int64_t start = synCount - S;
        if (start < 0)
            return;

        const int cp = (int) ((start + S / 2) & (synSize - 1));
        const float m = synMask[(size_t) cp], tgt = synTgt[(size_t) cp];
        const bool active = natural && m > 0.0f;
        // Synthesis index s holds input sample s - D (the detection delay).
        const bool learn  = params.algorithm != Params::Classic && m <= 0.0f && isRoomTone (start + S / 2 - D);
        if (! active && ! learn)
        {
            synActive = false;
            return;
        }

        const int half = S / 2;
        for (int c = 0; c < nch; ++c)
        {
            auto& a = amb[(size_t) c];
            auto& p = pwr[(size_t) c];
            for (int n = 0; n < S; ++n)
                sbuf[(size_t) n] = { syn[(size_t) c][(size_t) ((start + n) & (synSize - 1))] * sw[(size_t) n], 0.0f };
            sfft.forward (sbuf.data());

            for (int b = 0; b <= half; ++b)
            {
                const float e = std::norm (sbuf[(size_t) b]);
                if (learn)
                    a[(size_t) b] = ambCount[(size_t) c] == 0 ? e : a[(size_t) b] + 0.05f * (e - a[(size_t) b]);
                if (active)
                    p[(size_t) b] = synActive ? 0.5f * (p[(size_t) b] + e) : e;
            }
            if (learn && ambCount[(size_t) c] < 1000000)
                ++ambCount[(size_t) c];
            if (! active)
                continue;

            const bool haveAmb = ambCount[(size_t) c] > 0;
            for (int b = 0; b <= half; ++b)
                gbin[(size_t) b] = haveAmb ? std::max (tgt, std::sqrt (std::min (1.0f, a[(size_t) b] / std::max (p[(size_t) b], 1.0e-20f))))
                                           : tgt;
            for (int b = 0; b <= half; ++b) // light frequency smoothing against musical noise
                gsm[(size_t) b] = 0.25f * gbin[(size_t) std::max (0, b - 1)] + 0.5f * gbin[(size_t) b]
                                + 0.25f * gbin[(size_t) std::min (half, b + 1)];

            for (int b = 0; b <= half; ++b)
            {
                const float d = -m * (1.0f - gsm[(size_t) b]); // (gain - 1): build the difference signal
                sbuf[(size_t) b] *= d;
                if (b > 0 && b < half)
                    sbuf[(size_t) (S - b)] *= d;
            }
            sfft.inverse (sbuf.data());
            for (int n = 0; n < S; ++n)
                ola[(size_t) c][(size_t) ((start + n) & (synSize - 1))] += sbuf[(size_t) n].real() * sw[(size_t) n] * olaNorm;
        }
        synActive = active;
    }

    Params params;
    Stats stats;
    double fs = 44100.0, binHz = 0.0, winPow = 1.0;
    int N = 2048, H = 256, M = 4096, tauMin = 88, tauMax = 630;
    int Fscan = 0, G = 0, Rw = 0, R = 0, P = 0, lag = 0, D = 0;
    int anaSize = 0, delSize = 0, numCh = 0, anaPos = 0, delPos = 0, hopCount = 0;
    int64_t received = 0, frameCount = 0;

    FFT fft;
    std::vector<float> window, winAc, ana, frameData;
    std::vector<std::complex<float>> buf;
    std::vector<std::vector<float>> del;
    std::vector<Frame> frames;

    static constexpr int kSubs = 8;
    int subLen = 1, subCount = 0, subIdx = 0;
    float subMin = 0.0f, subMins[kSubs] {};
    float noiseFloorDb = -80.0f, speechDb = -24.0f;

    bool inBreath = false;
    int64_t segStart = 0, breathTotal = 0;
    std::atomic<float> meter { 0.0f };

    // Synthesis stage (Natural mode).
    FFT sfft;
    int S = 1024, Hs = 256, synSize = 0, synHop = 0;
    int64_t synCount = 0;
    bool synActive = false;
    float olaNorm = 0.5f;
    std::vector<float> sw, synMask, synTgt, synGain, gbin, gsm;
    std::vector<std::vector<float>> syn, ola, amb, pwr;
    std::vector<int> ambCount;
    std::vector<std::complex<float>> sbuf;
};

// Collects whole-file statistics for the Offline algorithm. Run one engine pass
// over the file with `engine.onDecision = [&] (auto, auto& f) { collector.add (f); }`,
// then give result() to the processing engine via setStats().
class StatsCollector
{
public:
    void add (const Engine::Frame& f)
    {
        if (f.levelDb > -100.0f)
            levels.push_back (f.levelDb);
        if (f.harm > 0.75f)
            voiced.push_back (f.levelDb);
    }

    Stats result() const
    {
        Stats s;
        if (levels.size() < 100)
            return s;
        s.floorDb = percentile (levels, 0.02f);
        std::vector<float> speech;
        for (float v : voiced)
            if (v > s.floorDb + 20.0f)
                speech.push_back (v);
        if (speech.size() >= 20)
            s.speechDb = percentile (speech, 0.5f);
        s.valid = true;
        return s;
    }

private:
    static float percentile (std::vector<float> v, float q)
    {
        const auto i = (size_t) (q * (float) (v.size() - 1));
        std::nth_element (v.begin(), v.begin() + (std::ptrdiff_t) i, v.end());
        return v[i];
    }

    std::vector<float> levels, voiced;
};
} // namespace bt
