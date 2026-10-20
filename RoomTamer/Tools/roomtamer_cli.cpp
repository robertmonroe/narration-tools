// roomtamer-cli: lay ACX-safe room tone under WAV files and print the ACX check,
// and the test harness for the shared engine. Usage:
//
//   roomtamer-cli in.wav out.wav [--level dB] [--tone -100..100] [--bypass]
//
// --bypass writes the file unchanged and only prints the ACX check.

#include "RoomEngine.h"

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



namespace
{
const char* word (rt::Verdict v) { return v == rt::Verdict::pass ? "PASS" : v == rt::Verdict::fail ? "FAIL" : "-"; }

std::string level (float db)
{
    if (db <= -199.0f)
        return "  silence";
    char buf[32];
    std::snprintf (buf, sizeof buf, "%6.2f dB", db);
    return buf;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: roomtamer-cli in.wav out.wav [--level dB] [--tone -100..100] [--bypass]\n";
        return 2;
    }

    rt::Params params;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string (argv[++i]) : std::string(); };
        if (a == "--level") params.levelDb = std::stof (next());
        else if (a == "--tone") params.tone = std::stof (next());
        else if (a == "--bypass") params.bypass = true;
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }

    Wav in;
    std::string err;
    if (! readWav (argv[1], in, err)) { std::cerr << "error: " << err << "\n"; return 1; }

    rt::Engine engine;
    engine.setParams (params);
    engine.prepare (in.sampleRate, in.channels);

    Wav out = in;
    const size_t len = in.data[0].size();
    constexpr size_t block = 4096;
    std::vector<float*> ptrs ((size_t) in.channels);
    for (size_t s = 0; s < len; s += block)
    {
        const int n = (int) std::min (block, len - s);
        for (int c = 0; c < in.channels; ++c)
            ptrs[(size_t) c] = out.data[(size_t) c].data() + s;
        engine.process (ptrs.data(), in.channels, n, -1, true);
    }
    if (! writeWav24 (argv[2], out)) { std::cerr << "error: can't write " << argv[2] << "\n"; return 1; }

    const rt::Reading r = engine.reading();
    std::printf ("ACX check (%.1f s, %s room tone at %.1f dB):\n", r.seconds, params.bypass ? "no" : "with",
                 params.levelDb);
    std::printf ("  noise floor  %s  %s  (-60 dB or lower, not dead silence)\n", level (r.floorDb).c_str(), word (rt::floorVerdict (r)));
    std::printf ("  RMS          %s  %s  (-23 to -18 dB)\n", level (r.rmsDb).c_str(), word (rt::rmsVerdict (r)));
    std::printf ("  peak         %s  %s  (-3 dB or lower)\n", level (r.peakDb).c_str(), word (rt::peakVerdict (r)));
    return 0;
}
