// breathtamer-cli: batch breath control for WAV files, and the tuning harness
// for the shared engine. Usage:
//
//   breathtamer-cli in.wav out.wav [--mode gain|target] [--level dB] [--sens 0-100]
//                   [--algorithm offline|realtime|classic] [--gated] [--strict-room-tone] [--breaths-only]
//                   [--regions r.csv] [--frames f.csv]
//
// --gain dB / --target dBFS are shorthands for --mode gain|target --level dB.
// The CLI defaults to the Offline algorithm (whole-file analysis, two passes).

#include "BreathEngine.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
struct Wav
{
    int sampleRate = 0, channels = 0;
    std::vector<std::vector<float>> data;
};

uint32_t u32 (const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }
uint16_t u16 (const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }

bool readWav (const std::string& path, Wav& w, std::string& err)
{
    std::ifstream f (path, std::ios::binary);
    if (! f) { err = "can't open " + path; return false; }
    std::vector<uint8_t> b ((std::istreambuf_iterator<char> (f)), {});
    if (b.size() < 12 || std::memcmp (b.data(), "RIFF", 4) || std::memcmp (b.data() + 8, "WAVE", 4))
    { err = "not a RIFF/WAVE file"; return false; }

    int tag = 0, bits = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmBytes = 0;
    for (size_t pos = 12; pos + 8 <= b.size();)
    {
        const uint32_t size = u32 (&b[pos + 4]);
        const uint8_t* body = &b[pos + 8];
        const size_t avail = b.size() - (pos + 8);
        if (! std::memcmp (&b[pos], "fmt ", 4) && size >= 16)
        {
            tag = u16 (body);
            w.channels = u16 (body + 2);
            w.sampleRate = (int) u32 (body + 4);
            bits = u16 (body + 14);
            if (tag == 0xFFFE && size >= 26)
                tag = u16 (body + 24);
        }
        else if (! std::memcmp (&b[pos], "data", 4))
        {
            pcm = body;
            pcmBytes = std::min<size_t> (size, avail);
            break;
        }
        pos += 8 + size + (size & 1);
    }

    if (pcm == nullptr || w.channels <= 0) { err = "missing fmt/data chunk"; return false; }
    if (! ((tag == 1 && (bits == 16 || bits == 24 || bits == 32)) || (tag == 3 && bits == 32)))
    { err = "unsupported WAV encoding (tag " + std::to_string (tag) + ", " + std::to_string (bits) + " bit)"; return false; }

    const size_t bps = (size_t) bits / 8, frames = pcmBytes / (bps * (size_t) w.channels);
    w.data.assign ((size_t) w.channels, std::vector<float> (frames));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < w.channels; ++c)
        {
            const uint8_t* p = pcm + (i * (size_t) w.channels + (size_t) c) * bps;
            float v;
            if (tag == 3) { float x; std::memcpy (&x, p, 4); v = x; }
            else if (bits == 16) v = (int16_t) u16 (p) / 32768.0f;
            else if (bits == 24) v = (int32_t) ((uint32_t) (p[0] << 8 | p[1] << 16 | (uint32_t) p[2] << 24)) / 2147483648.0f;
            else v = (int32_t) u32 (p) / 2147483648.0f;
            w.data[(size_t) c][i] = v;
        }
    return true;
}

bool writeWav24 (const std::string& path, const Wav& w)
{
    std::ofstream f (path, std::ios::binary);
    if (! f) return false;
    const uint32_t frames = (uint32_t) (w.data.empty() ? 0 : w.data[0].size());
    const uint32_t dataBytes = frames * (uint32_t) w.channels * 3;
    auto put32 = [&] (uint32_t v) { f.put ((char) v).put ((char) (v >> 8)).put ((char) (v >> 16)).put ((char) (v >> 24)); };
    auto put16 = [&] (uint16_t v) { f.put ((char) v).put ((char) (v >> 8)); };
    f.write ("RIFF", 4); put32 (36 + dataBytes); f.write ("WAVE", 4);
    f.write ("fmt ", 4); put32 (16); put16 (1); put16 ((uint16_t) w.channels); put32 ((uint32_t) w.sampleRate);
    put32 ((uint32_t) w.sampleRate * (uint32_t) w.channels * 3); put16 ((uint16_t) (w.channels * 3)); put16 (24);
    f.write ("data", 4); put32 (dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < w.channels; ++c)
        {
            const double x = std::max (-1.0, std::min (1.0, (double) w.data[(size_t) c][i]));
            const int32_t v = (int32_t) std::lround (x * 8388607.0);
            f.put ((char) v).put ((char) (v >> 8)).put ((char) (v >> 16));
        }
    return (bool) f;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: breathtamer-cli in.wav out.wav [--mode gain|target] [--level dB] [--sens 0-100]\n"
                     "                       [--algorithm offline|realtime|classic] [--gated] [--strict-room-tone] [--breaths-only]\n"
                     "                       [--regions r.csv] [--frames f.csv]\n"
                     "       --gain dB / --target dBFS are shorthands for --mode ... --level dB\n";
        return 2;
    }

    bt::Params params;
    params.algorithm = bt::Params::Offline; // whole files are available here
    std::string regionsPath, framesPath;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string (argv[++i]) : std::string(); };
        if (a == "--mode") params.mode = next() == "target" ? bt::Params::Target : bt::Params::Gain;
        else if (a == "--level") params.levelDb = std::stof (next());
        else if (a == "--gain") { params.mode = bt::Params::Gain; params.levelDb = std::stof (next()); }
        else if (a == "--target") { params.mode = bt::Params::Target; params.levelDb = std::stof (next()); }
        else if (a == "--sens") params.sensitivity = std::stof (next());
        else if (a == "--algorithm")
        {
            const auto v = next();
            params.algorithm = v == "realtime" ? bt::Params::RealTime : v == "classic" ? bt::Params::Classic : bt::Params::Offline;
        }
        else if (a == "--gated") params.natural = false;
        else if (a == "--natural") params.natural = true;
        else if (a == "--strict-room-tone") params.strictRoomTone = true;
        else if (a == "--breaths-only") params.breathsOnly = true;
        else if (a == "--regions") regionsPath = next();
        else if (a == "--frames") framesPath = next();
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }

    Wav in;
    std::string err;
    if (! readWav (argv[1], in, err)) { std::cerr << "error: " << err << "\n"; return 1; }

    const size_t len = in.data[0].size();

    // Streams the whole input (plus latency flush) through `e`; writes the
    // latency-compensated result to `out` when given.
    auto run = [&] (bt::Engine& e, Wav* out)
    {
        const size_t D = (size_t) e.latencySamples();
        constexpr size_t block = 4096;
        std::vector<std::vector<float>> scratch ((size_t) in.channels, std::vector<float> (block));
        std::vector<float*> ptrs ((size_t) in.channels);
        for (size_t pos = 0; pos < len + D; pos += block)
        {
            const size_t n = std::min (block, len + D - pos);
            for (int c = 0; c < in.channels; ++c)
            {
                for (size_t i = 0; i < n; ++i)
                    scratch[(size_t) c][i] = pos + i < len ? in.data[(size_t) c][pos + i] : 0.0f;
                ptrs[(size_t) c] = scratch[(size_t) c].data();
            }
            e.process (ptrs.data(), in.channels, (int) n);
            if (out != nullptr)
                for (int c = 0; c < in.channels; ++c)
                    for (size_t i = 0; i < n; ++i)
                        if (pos + i >= D && pos + i - D < len)
                            out->data[(size_t) c][pos + i - D] = scratch[(size_t) c][i];
        }
    };

    bt::Stats stats;
    if (params.algorithm == bt::Params::Offline)
    {
        bt::Engine pre;
        pre.prepare (in.sampleRate, in.channels);
        bt::Params detectOnly; // same pass as the app's analyseFileStats
        detectOnly.algorithm = bt::Params::Classic;
        pre.setParams (detectOnly);
        bt::StatsCollector collector;
        pre.onDecision = [&] (int64_t, const bt::Engine::Frame& f) { collector.add (f); };
        run (pre, nullptr);
        stats = collector.result();
        std::cout << "offline analysis: noise floor " << stats.floorDb << " dB, speech " << stats.speechDb
                  << " dB" << (stats.valid ? "" : " (too short, using running estimates)") << "\n";
    }

    bt::Engine engine;
    engine.prepare (in.sampleRate, in.channels);
    engine.setParams (params);
    engine.setStats (stats);

    std::vector<bt::Engine::Region> regions;
    engine.onRegion = [&] (const bt::Engine::Region& r) { regions.push_back (r); };

    std::ofstream frames;
    if (! framesPath.empty())
    {
        frames.open (framesPath);
        frames << "t,level,floor,speech,harm,centroid,hf,cand,blocked,breath\n";
        const int N = in.sampleRate <= 50000 ? 2048 : in.sampleRate <= 100000 ? 4096 : 8192, H = N / 8;
        engine.onDecision = [&, N, H] (int64_t k, const bt::Engine::Frame& f)
        {
            char line[256];
            std::snprintf (line, sizeof line, "%.4f,%.1f,%.1f,%.1f,%.3f,%.0f,%.3f,%d,%d,%d\n",
                           (double) ((k + 1) * H - N / 2) / in.sampleRate, f.levelDb, f.floorDb, f.speechDb, f.harm,
                           f.centroid, f.hfRatio, (int) f.cand, (int) f.blocked, (int) f.breath);
            frames << line;
        };
    }

    Wav out { in.sampleRate, in.channels, std::vector<std::vector<float>> ((size_t) in.channels, std::vector<float> (len)) };
    run (engine, &out);

    if (! writeWav24 (argv[2], out)) { std::cerr << "error: can't write " << argv[2] << "\n"; return 1; }

    if (! regionsPath.empty())
    {
        std::ofstream r (regionsPath);
        r << "start,end,duration\n";
        for (const auto& g : regions)
            r << (double) g.start / in.sampleRate << "," << (double) g.end / in.sampleRate << ","
              << (double) (g.end - g.start) / in.sampleRate << "\n";
    }

    std::cout << "breaths: " << engine.breathCount() << "  latency: " << engine.latencySamples() << " samples  length: "
              << (double) len / in.sampleRate << " s\n";
    return 0;
}
