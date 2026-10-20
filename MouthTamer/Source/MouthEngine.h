#pragma once
// MouthTamer DSP core: finds mouth clicks, lip pops and tongue clicks inside and
// around speech, and rebuilds the damaged samples from the speech on either side.
//
//   1. Detection runs on a high-passed mono copy (cutoff set by Frequency skew,
//      2 kHz at 0) whitened by short-term linear prediction. Speech, including its
//      smooth pitch pulses, is predictable; a click is not, so it stands out as a
//      spike in the prediction residual.
//   2. A spike is a click when it rises far above the local residual level (factor
//      set by Sensitivity) and lasts at most ~4 ms. Plosive releases out of a
//      closure and AI chunk starts out of digital silence are skipped.
//   3. Each click, plus the click widening, is replaced by least-squares
//      autoregressive interpolation from 20 ms of audio on both sides, per
//      channel. All other samples pass through unchanged.
//
// Plain C++17, shared by the plugin, the standalone app and the CLI.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace mt
{
struct Params
{
    float sensitivity = 50.0f; // 0..100
    float skew        = 0.0f;  // -100..100: detection band, lower (generic clicks) to higher
    float wideningMs  = 3.0f;  // extra repair after each click (lip-smack decay; ~3 ms measured)
    bool  clicksOnly  = false; // audition what is removed
    bool  bypass      = false; // latency-compensated dry signal
};

class Engine
{
public:
    struct Click
    {
        int64_t start = 0, end = 0; // repaired span, input-sample positions
        float ratio = 0.0f;         // spike height over the local residual level
    };

    // Optional hook for offline tools (not used on the audio thread).
    std::function<void (const Click&)> onClick;

    void prepare (double sampleRate, int maxChannels)
    {
        fs    = sampleRate;
        numCh = std::max (1, maxChannels);
        B     = std::max (16, (int) std::lround (fs * 0.010));
        C     = (int) std::lround (fs * 0.020);
        maxGap = (int) std::lround (fs * 0.008);
        D     = 6 * B; // a click in block b-4 is repaired when block b completes
        dec   = std::max (1, (int) std::lround (fs / 6000.0));

        size = 1;
        while (size < (int) (fs * 0.5))
            size <<= 1;
        buf.assign ((size_t) numCh, std::vector<float> ((size_t) size, 0.0f));
        orig.assign ((size_t) numCh, std::vector<float> ((size_t) size, 0.0f));
        mono.assign ((size_t) size, 0.0f);
        hp.assign ((size_t) size, 0.0f);
        res.assign ((size_t) size, 0.0f);
        ctxw.assign ((size_t) C, 0.0);
        win.resize ((size_t) (3 * B));
        for (int i = 0; i < 3 * B; ++i)
            win[(size_t) i] = 0.5 - 0.5 * std::cos (6.283185307179586 * (i + 0.5) / (3 * B));
        medBuf.reserve ((size_t) (2 * C / dec + 8));
        reset();
    }

    void reset()
    {
        for (auto* rings : { &buf, &orig })
            for (auto& r : *rings)
                std::fill (r.begin(), r.end(), 0.0f);
        std::fill (mono.begin(), mono.end(), 0.0f);
        std::fill (hp.begin(), hp.end(), 0.0f);
        std::fill (res.begin(), res.end(), 0.0f);
        for (auto& s : bq)
            s = {};
        received = 0;
        pending.clear();
        lastEnd = -1;
        total = 0;
        countOut.store (0);
        cutoff = -1.0;
    }

    void setParams (const Params& p) { params = p; }
    int latencySamples() const { return D; }
    int64_t clickCount() const { return total; }
    int64_t clickCountShared() const { return countOut.load(); } // safe to read from the UI

    void process (float* const* ch, int nch, int ns)
    {
        nch = std::min (nch, numCh);
        if (nch <= 0)
            return;
        updateFilter();

        for (int i = 0; i < ns; ++i)
        {
            const int w = idx (received);
            float m = 0.0f;
            for (int c = 0; c < nch; ++c)
            {
                buf[(size_t) c][(size_t) w] = orig[(size_t) c][(size_t) w] = ch[c][i];
                m += ch[c][i];
            }
            m /= (float) nch;
            mono[(size_t) w] = m;
            hp[(size_t) w] = (float) highPass (m);
            ++received;

            if (received % B == 0)
                blockReady (nch);

            const int64_t o = received - 1 - D;
            const int r = idx (o);
            for (int c = 0; c < nch; ++c)
            {
                const float x = buf[(size_t) c][(size_t) r], x0 = orig[(size_t) c][(size_t) r];
                ch[c][i] = o < 0 ? 0.0f : params.bypass ? x0 : params.clicksOnly ? x0 - x : x;
            }
        }
        countOut.store (total);
    }

private:
    static constexpr int P = 16;   // LPC order for detection
    static constexpr int Q = 24;   // clicks closer than this many samples are merged
    static constexpr int maxOrder = 200;

    int idx (int64_t n) const { return (int) (n & (int64_t) (size - 1)); }

    // ---- detection band: 4th-order Butterworth high-pass as two biquads ----
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; };
    Biquad bq[2];

    void updateFilter()
    {
        const double fc = std::min (0.40 * fs, 2000.0 * std::pow (2.0, params.skew / 50.0));
        if (std::abs (fc - cutoff) < 1.0)
            return;
        cutoff = fc;
        const double qs[2] = { 0.54119610, 1.30656296 };
        const double w0 = 6.283185307179586 * fc / fs, cw = std::cos (w0), sw = std::sin (w0);
        for (int s = 0; s < 2; ++s)
        {
            const double alpha = sw / (2.0 * qs[s]), a0 = 1.0 + alpha;
            bq[s].b0 = (1.0 + cw) / 2.0 / a0;
            bq[s].b1 = -(1.0 + cw) / a0;
            bq[s].b2 = (1.0 + cw) / 2.0 / a0;
            bq[s].a1 = -2.0 * cw / a0;
            bq[s].a2 = (1.0 - alpha) / a0;
        }
    }

    double highPass (double x)
    {
        for (auto& s : bq)
        {
            const double y = s.b0 * x + s.z1;
            s.z1 = s.b1 * x - s.a1 * y + s.z2;
            s.z2 = s.b2 * x - s.a2 * y;
            x = y;
        }
        return x;
    }

    // ---- per 10 ms block: whiten, detect, repair ----
    void blockReady (int nch)
    {
        const int64_t b = received / B - 1; // newest complete block
        if (b >= 1)
            whiten (b - 1);  // 30 ms window centred on block b-1
        if (b >= 4)
            detect (b - 4);  // needs residual for +-10 ms and audio 25 ms past each click
        repairDue (nch);
    }

    void whiten (int64_t blk)
    {
        const int64_t w0 = (blk - 1) * B; // window [w0, w0 + 3B)
        double r[P + 1] = {};
        for (int k = 0; k <= P; ++k)
            for (int n = k; n < 3 * B; ++n)
                r[k] += (double) hp[(size_t) idx (w0 + n)] * win[(size_t) n]
                      * (double) hp[(size_t) idx (w0 + n - k)] * win[(size_t) (n - k)];
        double a[P + 1];
        levinson (r, a, P);
        for (int64_t n = blk * B; n < (blk + 1) * B; ++n)
        {
            double e = hp[(size_t) idx (n)];
            for (int k = 1; k <= P; ++k)
                e += a[k] * hp[(size_t) idx (n - k)];
            res[(size_t) idx (n)] = (float) e;
        }
    }

    static void levinson (double* r, double* a, int order)
    {
        std::fill (a, a + order + 1, 0.0);
        a[0] = 1.0;
        if (r[0] <= 1.0e-18)
            return;
        r[0] *= 1.0001; // light regularisation
        double err = r[0];
        double tmp[maxOrder + 1];
        for (int i = 1; i <= order; ++i)
        {
            double acc = r[i];
            for (int j = 1; j < i; ++j)
                acc += a[j] * r[i - j];
            const double k = -acc / err;
            for (int j = 1; j < i; ++j)
                tmp[j] = a[j] + k * a[i - j];
            for (int j = 1; j < i; ++j)
                a[j] = tmp[j];
            a[i] = k;
            err *= 1.0 - k * k;
            if (err <= 0.0)
                break;
        }
    }

    double rms (int64_t a, int64_t b) const
    {
        double s = 0.0;
        for (int64_t n = a; n < b; ++n)
            s += (double) mono[(size_t) idx (n)] * mono[(size_t) idx (n)];
        return std::sqrt (s / (double) std::max<int64_t> (1, b - a));
    }

    void detect (int64_t blk)
    {
        const float u = std::clamp (params.sensitivity / 100.0f, 0.0f, 1.0f);
        const float K = 22.0f - 14.0f * u;
        const int ms = std::max (1, (int) std::lround (fs * 0.001));

        for (int64_t s0 = blk * B; s0 < (blk + 1) * B; s0 += ms)
        {
            // Robust local level of the residual: median over +-10 ms.
            medBuf.clear();
            for (int64_t n = s0 - C / 2; n < s0 + ms + C / 2; n += dec)
                medBuf.push_back (std::abs (res[(size_t) idx (n)]));
            std::nth_element (medBuf.begin(), medBuf.begin() + (std::ptrdiff_t) (medBuf.size() / 2), medBuf.end());
            const float scale = std::max (medBuf[medBuf.size() / 2], 1.0e-7f);

            for (int64_t n = s0; n < s0 + ms; ++n)
            {
                if (n <= lastEnd || std::abs (res[(size_t) idx (n)]) < K * scale)
                    continue;
                consider (n, scale);
            }
        }
    }

    void consider (int64_t n, float scale)
    {
        const int64_t reach = (int64_t) std::lround (fs * 0.003);
        int64_t a = n, e = n, peak = n;
        for (int64_t k = n; k <= n + reach; ++k) // find the peak just ahead
            if (std::abs (res[(size_t) idx (k)]) > std::abs (res[(size_t) idx (peak)]))
                peak = k;
        const float edge = 3.0f * scale;
        while (a > peak - reach && std::abs (res[(size_t) idx (a - 1)]) > edge) --a;
        while (e < peak + reach && std::abs (res[(size_t) idx (e + 1)]) > edge) ++e;
        lastEnd = e + (int64_t) std::lround (fs * 0.002);

        // A click is short; longer bursts are consonants.
        if ((double) (e - a) > fs * 0.004)
            return;
        // Not in near-silence noise.
        if (rms (a - (int64_t) (fs * 0.005), e + (int64_t) (fs * 0.005)) < 3.0e-4)
            return;
        // AI chunk start out of digital silence.
        int zeros = 0;
        for (int64_t k = a - (int64_t) (fs * 0.020); k < a; ++k)
            zeros += mono[(size_t) idx (k)] == 0.0f;
        if (zeros > (int) (fs * 0.001))
            return;
        // Plosive release out of a closure: much quieter just before than just after.
        const double before = rms (a - (int64_t) (fs * 0.025), a - (int64_t) (fs * 0.003));
        const double after  = rms (e + (int64_t) (fs * 0.002), e + (int64_t) (fs * 0.025));
        if (after > 10.0 * before && after > 1.0e-3)
            return;

        Click c;
        c.start = a - (int64_t) std::lround (fs * 0.00025);
        c.end = std::min (e + 1 + (int64_t) std::lround (fs * 0.001 * std::clamp (params.wideningMs, 0.0f, 5.0f)),
                          c.start + maxGap);
        c.ratio = std::abs (res[(size_t) idx (peak)]) / scale;
        if (! pending.empty() && c.start <= pending.back().end + Q)
            pending.back().end = std::min (std::max (pending.back().end, c.end), pending.back().start + maxGap);
        else
            pending.push_back (c);
        ++total;
        if (onClick)
            onClick (c);
    }

    void repairDue (int nch)
    {
        while (! pending.empty() && pending.front().end + C <= received)
        {
            for (int c = 0; c < nch; ++c)
                interpolate (buf[(size_t) c], pending.front().start, pending.front().end);
            pending.pop_front();
        }
    }

    // Least-squares AR interpolation of [g0, g1) from C samples of context each side.
    // The model order scales with the gap (3x its length): short models lose
    // accuracy fast on gaps over ~1 ms of voiced speech.
    void interpolate (std::vector<float>& x, int64_t g0, int64_t g1)
    {
        const int L = (int) (g1 - g0);
        if (L <= 0)
            return;
        const int O = std::clamp (3 * L, 24, std::min (maxOrder, C / 2));

        // AR model from the windowed context on both sides.
        std::vector<double> r ((size_t) O + 1, 0.0), a ((size_t) O + 1, 0.0), ra ((size_t) O + 1, 0.0);
        for (int side = 0; side < 2; ++side)
        {
            const int64_t s = side == 0 ? g0 - C : g1;
            for (int n = 0; n < C; ++n)
                ctxw[(size_t) n] = (double) x[(size_t) idx (s + n)] * (0.5 - 0.5 * std::cos (6.283185307179586 * (n + 0.5) / C));
            for (int k = 0; k <= O; ++k)
                for (int n = k; n < C; ++n)
                    r[(size_t) k] += ctxw[(size_t) n] * ctxw[(size_t) (n - k)];
        }
        levinson (r.data(), a.data(), O);

        // Normal equations M u = -d, with M Toeplitz from the autocorrelation of a.
        for (int dd = 0; dd <= O; ++dd)
            for (int k = 0; k + dd <= O; ++k)
                ra[(size_t) dd] += a[(size_t) k] * a[(size_t) (k + dd)];

        auto known = [&] (int64_t n) { return n < g0 || n >= g1; };
        std::vector<double> bvec ((size_t) (L + O), 0.0); // prediction error from known samples, rows g0..g1+O-1
        for (int row = 0; row < L + O; ++row)
        {
            const int64_t n = g0 + row;
            double s = 0.0;
            for (int k = 0; k <= O; ++k)
                if (known (n - k))
                    s += a[(size_t) k] * (double) x[(size_t) idx (n - k)];
            bvec[(size_t) row] = s;
        }
        std::vector<double> M ((size_t) L * (size_t) L, 0.0), d ((size_t) L, 0.0);
        for (int i = 0; i < L; ++i)
        {
            for (int j = 0; j < L; ++j)
            {
                const int dd = std::abs (i - j);
                M[(size_t) i * (size_t) L + (size_t) j] = dd <= O ? ra[(size_t) dd] : 0.0;
            }
            double s = 0.0;
            for (int k = 0; k <= O; ++k)
                s += a[(size_t) k] * bvec[(size_t) (i + k)];
            d[(size_t) i] = -s;
            M[(size_t) i * (size_t) L + (size_t) i] += 1.0e-9; // keep it positive definite
        }
        if (! choleskySolve (M, d, L))
            return;
        for (int i = 0; i < L; ++i)
            x[(size_t) idx (g0 + i)] = (float) d[(size_t) i];
    }

    static bool choleskySolve (std::vector<double>& A, std::vector<double>& b, int n)
    {
        for (int j = 0; j < n; ++j)
        {
            double s = A[(size_t) j * (size_t) n + (size_t) j];
            for (int k = 0; k < j; ++k)
                s -= A[(size_t) j * (size_t) n + (size_t) k] * A[(size_t) j * (size_t) n + (size_t) k];
            if (s <= 0.0)
                return false;
            const double l = std::sqrt (s);
            A[(size_t) j * (size_t) n + (size_t) j] = l;
            for (int i = j + 1; i < n; ++i)
            {
                double t = A[(size_t) i * (size_t) n + (size_t) j];
                for (int k = 0; k < j; ++k)
                    t -= A[(size_t) i * (size_t) n + (size_t) k] * A[(size_t) j * (size_t) n + (size_t) k];
                A[(size_t) i * (size_t) n + (size_t) j] = t / l;
            }
        }
        for (int i = 0; i < n; ++i) // forward: L y = b
        {
            double t = b[(size_t) i];
            for (int k = 0; k < i; ++k)
                t -= A[(size_t) i * (size_t) n + (size_t) k] * b[(size_t) k];
            b[(size_t) i] = t / A[(size_t) i * (size_t) n + (size_t) i];
        }
        for (int i = n - 1; i >= 0; --i) // backward: L^T x = y
        {
            double t = b[(size_t) i];
            for (int k = i + 1; k < n; ++k)
                t -= A[(size_t) k * (size_t) n + (size_t) i] * b[(size_t) k];
            b[(size_t) i] = t / A[(size_t) i * (size_t) n + (size_t) i];
        }
        return true;
    }

    Params params;
    double fs = 44100.0, cutoff = -1.0;
    int numCh = 1, B = 441, C = 882, maxGap = 352, D = 0, dec = 7, size = 0;
    int64_t received = 0, lastEnd = -1, total = 0;
    std::vector<std::vector<float>> buf, orig;
    std::vector<float> mono, hp, res, medBuf;
    std::vector<double> win, ctxw;
    std::deque<Click> pending;
    std::atomic<int64_t> countOut { 0 };
};
} // namespace mt
