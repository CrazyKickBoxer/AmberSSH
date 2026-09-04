// AmberXFuzzTests.cpp — persistent fuzz targets for the parsers that read
// bytes AmberSSH did not write. Phase 8 of docs/amberx/.
//
// These are the parsers a hostile peer can reach: the AmberXControl framing
// (a subverted host), the report and window metadata a host sends up, and the
// X11 setup packet AmberSSH inspects before it forwards a byte. Each target
// takes an arbitrary buffer and must do exactly one of two things: return a
// refusal, or return a value it can prove. Never read past the end, never
// allocate on an unchecked length, never crash.
//
// The driver is a seeded mutation loop rather than libFuzzer, so it runs
// everywhere the test suite runs, in CI, with no extra toolchain — and it is
// deterministic, so a failure is reproducible from the seed printed with it.
// With libFuzzer available (MSVC's /fsanitize=fuzzer or clang) the same
// FuzzOne functions can be driven by LLVMFuzzerTestOneInput; that is why they
// take a plain pointer and length.
//
// Corpus: tests/corpus/amberx/ holds the minimized cases that once broke
// something, one file each, with provenance in that directory's README. They
// are replayed first on every run, so a fixed bug stays fixed.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "amberx/control/Protocol.h"
#include "remote/XAuth.h"

using namespace amber::amberx;

namespace
{

// xorshift64*, so a failing iteration is reproducible from its seed alone.
struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t Next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint32_t Below(uint32_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0; }
};

// ---- the targets ---------------------------------------------------------
// Each returns true if it accepted the input, which the driver only uses to
// keep the corpus interesting. The contract being tested is that none of them
// misbehaves, whatever the answer.

bool FuzzFraming(const uint8_t* data, size_t len)
{
    Frame f;
    size_t consumed = 0;
    const std::vector<uint8_t> in(data, data + len);
    const Decoded d = Decode(in, f, consumed);
    if (d == Decoded::Ok)
    {
        // A frame that decoded must be re-encodable to the same bytes: a
        // decoder that accepts something its encoder cannot produce is a
        // decoder that has invented a message.
        std::vector<uint8_t> out;
        REQUIRE(Encode(f, out));
        REQUIRE(consumed <= in.size());
        REQUIRE(out.size() == consumed);
        REQUIRE(std::equal(out.begin(), out.end(), in.begin()));
        return true;
    }
    REQUIRE(consumed == 0);
    return false;
}

bool FuzzReport(const uint8_t* data, size_t len)
{
    HostReport r;
    if (!ParseHostReport(std::vector<uint8_t>(data, data + len), r))
        return false;
    // Everything it claims must be within the bounds it promised.
    REQUIRE(r.windowList.size() <= kMaxReportWindows);
    for (const ReportWindow& w : r.windowList)
        REQUIRE(w.title.size() <= kMaxWindowTitle);
    return true;
}

bool FuzzWindowAction(const uint8_t* data, size_t len)
{
    uint32_t xid = 0;
    WindowAct act = WindowAct::Show;
    if (!ParseWindowAction(std::vector<uint8_t>(data, data + len), xid, act))
        return false;
    REQUIRE(static_cast<uint32_t>(act) <= static_cast<uint32_t>(WindowAct::Close));
    return true;
}

bool FuzzHandshake(const uint8_t* data, size_t len)
{
    const std::vector<uint8_t> in(data, data + len);
    std::vector<uint8_t> a, b, c;
    uint32_t display = 0;
    bool any = ParseHello(in, a);
    any = ParseHelloAck(in, a, b, c) || any;
    any = ParseAuthProof(in, a) || any;
    any = ParseSetCookie(in, a, display) || any;
    uint32_t open = 0;
    uint64_t bytes = 0;
    bool cookie = false;
    any = ParseHostStatus(in, open, bytes, cookie) || any;
    std::string text;
    any = ParseHostError(in, text) || any;
    REQUIRE(text.size() <= kMaxErrorBytes);
    return any;
}

// The X11 setup packet, as AmberSSH inspects it before forwarding: this one
// reads bytes straight off an SSH channel from a remote machine, which makes
// it the most exposed parser in the whole feature.
bool FuzzSetup(const uint8_t* data, size_t len)
{
    const std::vector<uint8_t> in(data, data + len);
    const std::vector<uint8_t> fake(16, 0xAB), real(16, 0xCD);
    std::vector<uint8_t> out;
    const amber::XAuthVerdict v = amber::RewriteSetup(in, fake, real, out);
    if (v == amber::XAuthVerdict::Rewritten)
    {
        // A rewritten packet is the same length as the one that came in:
        // the cookie is substituted, never grown.
        REQUIRE(out.size() == in.size());
        return true;
    }
    REQUIRE(out.empty());
    return false;
}

using Target = bool (*)(const uint8_t*, size_t);

struct TargetInfo
{
    const char* name;
    Target fn;
};

const TargetInfo kTargets[] = {
    { "framing", FuzzFraming },
    { "report", FuzzReport },
    { "window-action", FuzzWindowAction },
    { "handshake", FuzzHandshake },
    { "setup", FuzzSetup },
};

// ---- seeds ---------------------------------------------------------------
// Well-formed messages to mutate. Starting from noise alone, a length-prefixed
// format is almost never reached past its first check.
std::vector<std::vector<uint8_t>> Seeds()
{
    std::vector<std::vector<uint8_t>> out;

    Frame f;
    f.type = MsgType::ChannelData;
    f.channel = 1;
    f.payload = { 'B', 0, 0, 11, 0, 0 };
    std::vector<uint8_t> enc;
    Encode(f, enc);
    out.push_back(enc);

    HostReport r;
    r.clients = 1;
    r.windows = 2;
    r.windowList.push_back({ 0x200001u, 4u, "xterm" });
    out.push_back(MakeHostReport(r));
    out.push_back(MakeWindowAction(0x200001u, WindowAct::Minimize));
    out.push_back(MakeHostStatus(1, 4096, true));
    out.push_back(MakeSetCookie(std::vector<uint8_t>(16, 0xAB), 0));

    // An X11 setup packet with a 16-byte MIT-MAGIC-COOKIE-1.
    {
        std::vector<uint8_t> s{ 'l', 0, 11, 0, 0, 0, 18, 0, 16, 0, 0, 0 };
        const char* name = "MIT-MAGIC-COOKIE-1";
        s.insert(s.end(), name, name + 18);
        s.push_back(0);
        s.push_back(0);
        s.insert(s.end(), 16, 0xAB);
        out.push_back(s);
    }
    return out;
}

// One mutation: the classic set, kept small on purpose. A mutator that is
// cleverer than the format is a mutator that stops reaching the parser.
void Mutate(std::vector<uint8_t>& b, Rng& rng)
{
    if (b.empty())
    {
        b.push_back(static_cast<uint8_t>(rng.Next()));
        return;
    }
    switch (rng.Below(6))
    {
    case 0: b[rng.Below(static_cast<uint32_t>(b.size()))] ^= static_cast<uint8_t>(1u << rng.Below(8)); break;
    case 1: b[rng.Below(static_cast<uint32_t>(b.size()))] = static_cast<uint8_t>(rng.Next()); break;
    case 2: b.insert(b.begin() + rng.Below(static_cast<uint32_t>(b.size()) + 1),
                     static_cast<uint8_t>(rng.Next())); break;
    case 3: b.erase(b.begin() + rng.Below(static_cast<uint32_t>(b.size()))); break;
    case 4:
        // A length field pushed to an extreme: the mutation that matters most
        // for a length-prefixed format.
        if (b.size() >= 4)
        {
            const uint32_t at = rng.Below(static_cast<uint32_t>(b.size()) - 3);
            const uint32_t v = rng.Below(3) == 0 ? 0xFFFFFFFFu : rng.Below(2) ? 0x7FFFFFFFu : 0;
            for (int i = 0; i < 4; ++i)
                b[at + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
        }
        break;
    default: b.resize(rng.Below(4096)); break;
    }
    if (b.size() > 8192)
        b.resize(8192);
}

std::filesystem::path CorpusDir()
{
    // tests/corpus/amberx relative to the source tree; the tests run from the
    // build directory, so the path is resolved from the macro the build sets,
    // and a missing directory simply means "no regression cases yet".
#ifdef AMBER_TEST_SOURCE_DIR
    return std::filesystem::path(AMBER_TEST_SOURCE_DIR) / "corpus" / "amberx";
#else
    return std::filesystem::path("tests") / "corpus" / "amberx";
#endif
}

} // namespace

TEST_CASE("the AmberX corpus replays without incident", "[amberx][fuzz]")
{
    const std::filesystem::path dir = CorpusDir();
    if (!std::filesystem::exists(dir))
    {
        WARN("no corpus directory at " << dir.string() << " — nothing to replay");
        return;
    }
    size_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir))
    {
        if (!e.is_regular_file() || e.path().extension() == ".md")
            continue;
        std::ifstream in(e.path(), std::ios::binary);
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        ++files;
        // Every case goes to every target: a case minimized for one parser
        // has repeatedly turned out to be interesting to another.
        for (const TargetInfo& t : kTargets)
            t.fn(data.data(), data.size());
    }
    CHECK(files >= 0);
}

TEST_CASE("the AmberX parsers survive a mutation run", "[amberx][fuzz]")
{
    // Deterministic: the seed is fixed, so a failure here is reproducible by
    // running this test again, and the iteration count is the knob for a
    // longer soak (AMBERX_FUZZ_ITERS in the environment).
    uint64_t seed = 0xA3B1C2D4E5F60718ull;
    int iters = 20000;
    if (const char* env = std::getenv("AMBERX_FUZZ_ITERS"))
        iters = std::max(100, std::atoi(env));
    if (const char* env = std::getenv("AMBERX_FUZZ_SEED"))
        seed = std::strtoull(env, nullptr, 0);
    INFO("seed 0x" << std::hex << seed << " iterations " << std::dec << iters);

    Rng rng(seed);
    std::vector<std::vector<uint8_t>> pool = Seeds();
    int accepted = 0;
    for (int i = 0; i < iters; ++i)
    {
        std::vector<uint8_t> b = pool[rng.Below(static_cast<uint32_t>(pool.size()))];
        const int rounds = 1 + static_cast<int>(rng.Below(4));
        for (int m = 0; m < rounds; ++m)
            Mutate(b, rng);
        const TargetInfo& t = kTargets[rng.Below(static_cast<uint32_t>(std::size(kTargets)))];
        if (t.fn(b.data(), b.size()))
        {
            ++accepted;
            // Keep a bounded pool of inputs that got past the first checks,
            // so later iterations start from deeper in the format.
            if (pool.size() < 64)
                pool.push_back(b);
        }
    }
    // Not a correctness assertion — a run where nothing was ever accepted
    // would mean the mutator never reached a parser, which is worth knowing.
    CHECK(accepted > 0);
}
