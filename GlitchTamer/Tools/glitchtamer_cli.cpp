// glitchtamer-cli: repair AI-voice seam glitches (clicks/pops in pauses) in WAV
// files, and the tuning harness for the shared engine. Usage:
//
//   glitchtamer-cli in.wav out.wav [--sens 0-100] [--width ms] [--glitches-only]
//                   [--report glitches.csv]

#include "GlitchEngine.h"

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
        std::cerr << "usage: glitchtamer-cli in.wav out.wav [--sens 0-100] [--width ms] [--glitches-only]\n"
                     "                       [--report glitches.csv]\n";
        return 2;
    }

    gt::Params params;
    std::string reportPath;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string (argv[++i]) : std::string(); };
        if (a == "--sens") params.sensitivity = std::stof (next());
        else if (a == "--width") params.widthMs = std::stof (next());
        else if (a == "--glitches-only") params.glitchesOnly = true;
        else if (a == "--report") reportPath = next();
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }

    Wav in;
    std::string err;
    if (! readWav (argv[1], in, err)) { std::cerr << "error: " << err << "\n"; return 1; }

    gt::Engine engine;
    engine.prepare (in.sampleRate, in.channels);
    engine.setParams (params);
    std::vector<gt::Engine::Glitch> glitches;
    engine.onGlitch = [&] (const gt::Engine::Glitch& g) { glitches.push_back (g); };

    const size_t len = in.data[0].size(), D = (size_t) engine.latencySamples();
    Wav out { in.sampleRate, in.channels, std::vector<std::vector<float>> ((size_t) in.channels, std::vector<float> (len)) };
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
        engine.process (ptrs.data(), in.channels, (int) n);
        for (int c = 0; c < in.channels; ++c)
            for (size_t i = 0; i < n; ++i)
                if (pos + i >= D && pos + i - D < len)
                    out.data[(size_t) c][pos + i - D] = scratch[(size_t) c][i];
    }

    if (! writeWav24 (argv[2], out)) { std::cerr << "error: can't write " << argv[2] << "\n"; return 1; }

    const double sr = in.sampleRate;
    if (! reportPath.empty())
    {
        std::ofstream r (reportPath);
        r << "type,peak_s,start_s,end_s,level_db,rise_db\n";
        for (const auto& g : glitches)
            r << (g.burst ? "static" : "click") << "," << g.peak / sr << "," << g.start / sr << "," << g.end / sr
              << "," << g.levelDb << "," << g.jumpDb << "\n";
    }

    std::cout << "glitches repaired: " << engine.glitchCount() << "  latency: " << D << " samples  length: "
              << len / sr << " s\n";
    for (const auto& g : glitches)
    {
        char line[160];
        std::snprintf (line, sizeof line, "  %-6s %8.3f s  (%.3f-%.3f)  peak %6.1f dB  +%4.1f dB above surroundings\n",
                       g.burst ? "static" : "click", g.peak / sr, g.start / sr, g.end / sr, g.levelDb, g.jumpDb);
        std::cout << line;
    }
    return 0;
}
