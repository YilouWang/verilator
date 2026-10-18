// rtlfast oracle harness.
//
// The same source is compiled against the reference model (stock Verilator)
// and against the model under test (the rtlfast backend). Both builds use the
// prefix "Vm" and run as separate processes, so their runtimes never meet.
//
// Generated include files (made by gen_probes.py from BOTH models' headers):
//   probes.inc   P(name, width)    design state members present in both models
//   outputs.inc  O(name, width)    top-level output ports
//   inputs.inc   I(name, width)    top-level input ports driven with seeded random data
//   preload.inc  PRELOAD(member, path)
//   design.inc   #define CLOCK_PORT / RESET_PORT / RESET_ACTIVE
//
// Modes:
//   digest    write one 64-bit state digest per cycle to --out
//   dump      write "name hash" for every probe at --dump-cycle to --out
//   perf      run without probing and report cycles per second
//   activity  report the fraction of probed members that change per cycle

#include "Vm.h"
#include "Vm___024root.h"
#include "verilated.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "design.inc"

#ifndef BIG_BYTES
#define BIG_BYTES 4096
#endif

static inline uint64_t mix64(uint64_t h, uint64_t v) {
    v *= 0x9e3779b97f4a7c15ULL;
    v ^= v >> 29;
    h ^= v;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 31;
    return h;
}

static inline uint64_t hash_bytes(const void* p, size_t n, uint64_t h) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, b + i, 8);
        h = mix64(h, v);
    }
    uint64_t tail = 0;
    if (i < n) {
        std::memcpy(&tail, b + i, n - i);
        h = mix64(h, tail ^ (uint64_t(n - i) << 56));
    }
    return mix64(h, n);
}

// Probe table. Each entry points at one design state member. Building the
// table at runtime (instead of expanding hashing code per member) keeps the
// harness cheap to compile even for designs with tens of thousands of members.
struct Probe {
    const char* name;
    const void* ptr;
    size_t size;
    int width;      // declared width, 0 if unknown
    bool integral;  // CData/SData/IData/QData: mask to width before hashing
};

static inline uint64_t hash_probe(const Probe& p, uint64_t h) {
    if (p.integral) {
        uint64_t v = 0;
        std::memcpy(&v, p.ptr, p.size);
        if (p.width > 0 && p.width < 64) v &= ((uint64_t(1) << p.width) - 1);
        return mix64(h, v);
    }
    return hash_bytes(p.ptr, p.size, h);
}

template <typename T>
static inline Probe make_probe(const char* name, const T& m, int width) {
    return Probe{name, &m, sizeof(T), width, std::is_integral<T>::value};
}

struct Rng {
    uint64_t s;
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

template <typename T>
static inline void drive_random(T& port, int width, Rng& rng) {
    if constexpr (std::is_integral<T>::value) {
        uint64_t v = rng.next();
        if (width < 64) v &= ((uint64_t(1) << width) - 1);
        port = static_cast<T>(v);
    } else {
        // VlWide
        const size_t words = sizeof(port) / sizeof(EData);
        for (size_t i = 0; i < words; ++i) port[i] = static_cast<EData>(rng.next());
        if (width % 32) port[words - 1] &= ((EData(1) << (width % 32)) - 1);
    }
}

static void preload_member(void* dst, size_t dst_bytes, const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "rtlfast: cannot open preload file %s\n", path);
        std::exit(2);
    }
    size_t n = std::fread(dst, 1, dst_bytes, f);
    std::fclose(f);
    std::fprintf(stderr, "rtlfast: preloaded %zu bytes from %s\n", n, path);
}

static void build_tables(Vm* m, std::vector<Probe>& state, std::vector<Probe>& outs) {
    Vm___024root* r = m->rootp;
#define P(name, width) state.push_back(make_probe(#name, r->name, width));
#include "probes.inc"
#undef P
#define O(name, width) outs.push_back(make_probe("PORT." #name, m->name, width));
#include "outputs.inc"
#undef O
    (void)r;
}

static uint64_t state_digest(const std::vector<Probe>& state, const std::vector<Probe>& outs,
                             bool full) {
    uint64_t h = 0x1234567;
    for (const Probe& p : state)
        if (full || p.size <= BIG_BYTES) h = hash_probe(p, h);
    for (const Probe& p : outs) h = hash_probe(p, h);
    return h;
}

static void dump_probes(const std::vector<Probe>& state, const std::vector<Probe>& outs, FILE* out) {
    for (const Probe& p : state)
        std::fprintf(out, "%s %016llx\n", p.name, (unsigned long long)hash_probe(p, 0));
    for (const Probe& p : outs)
        std::fprintf(out, "%s %016llx\n", p.name, (unsigned long long)hash_probe(p, 0));
}

int main(int argc, char** argv) {
    std::string mode = "digest", out_path = "digest.bin";
    long cycles = 100000, reset_cycles = 10, dump_cycle = -1, big_every = 1000;
    uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--mode") mode = val();
        else if (a == "--out") out_path = val();
        else if (a == "--cycles") cycles = std::atol(val().c_str());
        else if (a == "--reset-cycles") reset_cycles = std::atol(val().c_str());
        else if (a == "--seed") seed = std::strtoull(val().c_str(), nullptr, 0);
        else if (a == "--dump-cycle") dump_cycle = std::atol(val().c_str());
        else if (a == "--big-every") big_every = std::atol(val().c_str());
    }

    VerilatedContext* ctx = new VerilatedContext;
    ctx->randSeed(static_cast<int>(seed));
    Vm* m = new Vm{ctx};
    Rng rng{seed * 0x2545F4914F6CDD1DULL + 1};

#define PRELOAD(member, path) preload_member(&m->rootp->member, sizeof(m->rootp->member), path);
#include "preload.inc"
#undef PRELOAD

    FILE* out = nullptr;
    if (mode == "digest" || mode == "dump") {
        out = std::fopen(out_path.c_str(), mode == "digest" ? "wb" : "w");
        if (!out) { std::perror("rtlfast: open --out"); return 2; }
    }

    std::vector<Probe> state, outs;
    build_tables(m, state, outs);
    const size_t nprobe = state.size();
    std::vector<uint64_t> prev(nprobe), cur(nprobe);
    double changed_sum = 0;
    long activity_cycles = 0;

    auto t0 = std::chrono::steady_clock::now();
    long perf_start_cycle = reset_cycles;
    auto tperf = t0;
    for (long cyc = 0; cyc < cycles && !ctx->gotFinish(); ++cyc) {
        if (cyc == perf_start_cycle) tperf = std::chrono::steady_clock::now();
        m->RESET_PORT = (cyc < reset_cycles) ? RESET_ACTIVE : !RESET_ACTIVE;
#define I(name, width) drive_random(m->name, width, rng);
#include "inputs.inc"
#undef I
        m->CLOCK_PORT = 0;
        m->eval();
        m->CLOCK_PORT = 1;
        m->eval();

        if (mode == "digest") {
            bool full = (big_every > 0 && cyc % big_every == 0) || cyc == cycles - 1;
            uint64_t d = state_digest(state, outs, full);
            std::fwrite(&d, sizeof d, 1, out);
        } else if (mode == "dump" && cyc == dump_cycle) {
            dump_probes(state, outs, out);
            break;
        } else if (mode == "activity" && cyc >= reset_cycles) {
            for (size_t k = 0; k < nprobe; ++k)
                cur[k] = state[k].size <= BIG_BYTES ? hash_probe(state[k], 0) : 0;
            if (cyc > reset_cycles) {
                size_t changed = 0;
                for (size_t k = 0; k < nprobe; ++k) changed += (cur[k] != prev[k]);
                changed_sum += double(changed) / double(nprobe ? nprobe : 1);
                ++activity_cycles;
            }
            std::swap(prev, cur);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - tperf).count();
    long ran = ctx->gotFinish() ? -1 : cycles;

    if (mode == "perf") {
        long measured = cycles - perf_start_cycle;
        std::printf("RTLFAST_PERF cycles=%ld seconds=%.3f cycles_per_sec=%.1f\n", measured, secs,
                    measured / (secs > 0 ? secs : 1e-9));
    } else if (mode == "activity") {
        std::printf("RTLFAST_ACTIVITY probes=%zu cycles=%ld mean_changed_fraction=%.5f\n", nprobe,
                    activity_cycles, activity_cycles ? changed_sum / activity_cycles : 0.0);
    }
    std::printf("RTLFAST_DONE mode=%s cycles=%ld finished_early=%d probes=%zu\n", mode.c_str(), ran,
                ctx->gotFinish() ? 1 : 0, nprobe);
    if (out) std::fclose(out);
    m->final();
    delete m;
    delete ctx;
    return 0;
}
