// Synthetic layered-DAG generator for scaling studies.
//
//   gen_random_dag --layers L --width W [--fanin-min 2] [--fanin-max 4] [--seed S] [--out FILE]
//
// Layer 0 is W primary inputs; layers 1..L each hold W gates. A gate reads 2-4 distinct
// nets, mostly from the previous layer (75%) and otherwise from any earlier layer, so
// the logic depth is exactly L. Every gate nobody reads becomes an OUTPUT, so there is
// no dead logic. Output is ISCAS .bench, deterministic for a given seed.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

[[noreturn]] void usage(const char* msg) {
    std::fprintf(stderr,
                 "gen_random_dag: %s\n"
                 "usage: gen_random_dag --layers L --width W [--fanin-min 2] [--fanin-max 4] "
                 "[--seed S] [--out FILE]\n",
                 msg);
    std::exit(64);
}

std::uint64_t parseUInt(const char* s, const char* what) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0') usage(what);
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t layers = 0, width = 0, faninMin = 2, faninMax = 4, seed = 1;
    const char* outPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) usage(what);
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--layers")) layers = parseUInt(next("--layers needs a value"), "bad --layers");
        else if (!std::strcmp(argv[i], "--width")) width = parseUInt(next("--width needs a value"), "bad --width");
        else if (!std::strcmp(argv[i], "--fanin-min")) faninMin = parseUInt(next("--fanin-min needs a value"), "bad --fanin-min");
        else if (!std::strcmp(argv[i], "--fanin-max")) faninMax = parseUInt(next("--fanin-max needs a value"), "bad --fanin-max");
        else if (!std::strcmp(argv[i], "--seed")) seed = parseUInt(next("--seed needs a value"), "bad --seed");
        else if (!std::strcmp(argv[i], "--out")) outPath = next("--out needs a path");
        else usage("unknown option");
    }
    if (layers == 0 || width == 0) usage("--layers and --width are required and must be >= 1");
    if (faninMin < 2 || faninMax < faninMin) usage("need 2 <= fanin-min <= fanin-max");

    std::mt19937_64 rng(seed);
    static const char* kTypes[] = {"AND", "NAND", "OR", "NOR", "XOR", "XNOR"};

    const std::uint64_t total = width * (layers + 1);  // inputs + gates
    std::vector<char> read(total, 0);                  // does anyone read net i?
    std::string body;
    body.reserve(total * 40);

    auto name = [&](std::uint64_t id) {
        return (id < width ? "i" + std::to_string(id) : "g" + std::to_string(id - width));
    };

    std::vector<std::uint64_t> picks;
    for (std::uint64_t layer = 1; layer <= layers; ++layer) {
        const std::uint64_t prevBegin = (layer - 1) * width;  // first net of the previous layer
        const std::uint64_t curBegin = layer * width;
        for (std::uint64_t k = 0; k < width; ++k) {
            const std::uint64_t self = curBegin + k;
            std::uint64_t count = faninMin + rng() % (faninMax - faninMin + 1);
            if (count > curBegin) count = curBegin;  // fewer than `count` earlier nets exist
            picks.clear();
            while (picks.size() < count) {
                std::uint64_t cand;
                if (rng() % 4 != 0) {
                    cand = prevBegin + rng() % width;  // previous layer
                } else {
                    cand = rng() % curBegin;  // any earlier net
                }
                bool dup = false;
                for (std::uint64_t p : picks) dup = dup || p == cand;
                if (!dup) picks.push_back(cand);
            }
            body += name(self);
            body += " = ";
            body += kTypes[rng() % 6];
            body += '(';
            for (std::size_t j = 0; j < picks.size(); ++j) {
                if (j) body += ", ";
                body += name(picks[j]);
                read[picks[j]] = 1;
            }
            body += ")\n";
        }
    }

    std::string head;
    head.reserve(width * 12 + 64);
    for (std::uint64_t i = 0; i < width; ++i) head += "INPUT(" + name(i) + ")\n";
    std::uint64_t outputs = 0;
    for (std::uint64_t id = width; id < total; ++id) {
        if (!read[id]) {
            head += "OUTPUT(" + name(id) + ")\n";
            ++outputs;
        }
    }

    std::FILE* f = outPath ? std::fopen(outPath, "wb") : stdout;
    if (!f) {
        std::fprintf(stderr, "gen_random_dag: cannot write %s\n", outPath);
        return 2;
    }
    std::fprintf(f, "# synthetic layered DAG: layers=%llu width=%llu fanin=%llu..%llu seed=%llu\n",
                 static_cast<unsigned long long>(layers), static_cast<unsigned long long>(width),
                 static_cast<unsigned long long>(faninMin), static_cast<unsigned long long>(faninMax),
                 static_cast<unsigned long long>(seed));
    std::fwrite(head.data(), 1, head.size(), f);
    std::fwrite(body.data(), 1, body.size(), f);
    if (outPath) std::fclose(f);
    std::fprintf(stderr, "gen_random_dag: %llu gates, %llu inputs, %llu outputs\n",
                 static_cast<unsigned long long>(layers * width), static_cast<unsigned long long>(width),
                 static_cast<unsigned long long>(outputs));
    return 0;
}
