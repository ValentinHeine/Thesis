// Prototype: SIMD structural indexing vs. RapidXML for schema-guided field extraction.
//
// Both variants perform the identical job of NES's XMLInputFormatIndexer fast path: walk a
// delimiter-framed XML record stream and, for every record, emit a (start,end) byte-offset pair
// per schema field, relative to the start of the raw buffer.
//
// Variant A (rapidxml): copy record -> null-terminate -> parse tree -> walk children.
// Variant B (simd):     AVX2 scan of the record into a list of '<' / '>' positions -> walk pairs.
//
// The checksum over all emitted offsets must agree exactly, or the comparison is meaningless.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <immintrin.h>

#include "rapidxml.hpp"

namespace
{
constexpr char DELIM = '\n';
using OffsetPair = std::pair<uint32_t, uint32_t>;

const std::vector<std::string> kFields = {
    "AGENCIA_ID", "CANAL_ID", "CLIENTE_ID", "DEMANDA_UNI_EQUIL", "DEV_PROXIMA", "DEV_UNI_PROXIMA",
    "NUMBER_OF_RECORDS", "PRODUCTO_ID", "RUTA_SAK", "SEMANA", "VENTA_HOY", "VENTA_UNI_HOY"};

struct SvHash
{
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    size_t operator()(const std::string& s) const noexcept { return std::hash<std::string_view>{}(s); }
};
using FieldMap = std::unordered_map<std::string, uint32_t, SvHash, std::equal_to<>>;

inline void emit(std::vector<OffsetPair>& out, size_t& checksum)
{
    for (const auto& p : out) { checksum += p.first + p.second; }
}

// ------------------------- Variant A: RapidXML -------------------------
size_t runRapid(std::string_view buf, const FieldMap& fm, size_t nFields)
{
    thread_local std::vector<char> tl;
    rapidxml::xml_document<> doc;
    std::vector<OffsetPair> out(nFields);
    size_t checksum = 0;

    size_t s = buf.find(DELIM);
    if (s == std::string_view::npos) { return 0; }
    s += 1;
    size_t e = buf.find(DELIM, s);
    while (e != std::string_view::npos)
    {
        const auto rec = buf.substr(s, e - s);
        if (rec.find_first_not_of(" \t\r\n") != std::string_view::npos)
        {
            tl.resize(rec.size() + 1);
            std::memcpy(tl.data(), rec.data(), rec.size());
            tl[rec.size()] = '\0';
            doc.clear();
            doc.parse<rapidxml::parse_non_destructive | rapidxml::parse_no_entity_translation>(tl.data());
            rapidxml::xml_node<>* root = doc.first_node();
            if (root != nullptr)
            {
                const auto ts = static_cast<uint32_t>(s);
                std::fill(out.begin(), out.end(), OffsetPair{ts, ts});
                size_t found = 0;
                for (auto* n = root->first_node(); n != nullptr && found < nFields; n = n->next_sibling())
                {
                    auto it = fm.find(std::string_view(n->name(), n->name_size()));
                    if (it == fm.end()) { continue; }
                    auto& slot = out[it->second];
                    if (slot.first != ts || slot.second != ts) { continue; }
                    const auto off = static_cast<uint32_t>(n->value() - tl.data());
                    slot = {ts + off, ts + off + static_cast<uint32_t>(n->value_size())};
                    ++found;
                }
                emit(out, checksum);
            }
        }
        s = e + 1;
        e = buf.find(DELIM, s);
    }
    return checksum;
}

// ------------------------- Variant B: SIMD structural -------------------------
//
// Stage 1: one AVX2 pass over the record collecting every '<' and '>' position.
// Stage 2: walk that list. For a flat record <root><A>v</A><B>v</B></root> the positions after the
// root's open tag arrive in groups of four: '<' and '>' of the open tag, then '<' and '>' of the
// close tag. The element name is between the first pair and the value between them.
//
// This exploits the same shape as the indexer's bare-name fast path (direct child elements, no
// attributes). It is a prototype of stage 1, not a general XML parser.
inline void scanStructurals(const char* p, size_t len, std::vector<uint32_t>& pos)
{
    pos.clear();
    const __m256i lt = _mm256_set1_epi8('<');
    const __m256i gt = _mm256_set1_epi8('>');
    size_t i = 0;
    for (; i + 32 <= len; i += 32)
    {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
        uint32_t m = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, lt)))
                   | static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, gt)));
        while (m) { pos.push_back(static_cast<uint32_t>(i) + __builtin_ctz(m)); m &= m - 1; }
    }
    for (; i < len; ++i) { if (p[i] == '<' || p[i] == '>') { pos.push_back(static_cast<uint32_t>(i)); } }
}

size_t runSimd(std::string_view buf, const FieldMap& fm, size_t nFields)
{
    std::vector<uint32_t> pos;
    pos.reserve(256);
    std::vector<OffsetPair> out(nFields);
    size_t checksum = 0;

    size_t s = buf.find(DELIM);
    if (s == std::string_view::npos) { return 0; }
    s += 1;
    size_t e = buf.find(DELIM, s);
    while (e != std::string_view::npos)
    {
        const char* rec = buf.data() + s;
        const size_t len = e - s;
        if (std::string_view(rec, len).find_first_not_of(" \t\r\n") != std::string_view::npos)
        {
            scanStructurals(rec, len, pos);
            const auto ts = static_cast<uint32_t>(s);
            std::fill(out.begin(), out.end(), OffsetPair{ts, ts});
            size_t found = 0;
            // pos[0],pos[1] delimit the root open tag; children follow in groups of four.
            for (size_t k = 2; k + 3 < pos.size() && found < nFields; k += 4)
            {
                const uint32_t ltOpen = pos[k], gtOpen = pos[k + 1], ltClose = pos[k + 2];
                if (rec[ltOpen + 1] == '/') { break; }          // reached </root>
                auto it = fm.find(std::string_view(rec + ltOpen + 1, gtOpen - ltOpen - 1));
                if (it == fm.end()) { continue; }
                auto& slot = out[it->second];
                if (slot.first != ts || slot.second != ts) { continue; }
                slot = {ts + gtOpen + 1, ts + ltClose};
                ++found;
            }
            emit(out, checksum);
        }
        s = e + 1;
        e = buf.find(DELIM, s);
    }
    return checksum;
}
// ------------------------- Variant C: SIMD + direct-mapped name dispatch -------------------------
//
// The profile of variant B is dominated by hashing element names. A schema has a small, fixed set
// of names, so a name can be dispatched on (length, first byte) through a direct-mapped table and
// confirmed with one memcmp -- no string hashing at all.
struct FastMatcher
{
    static constexpr size_t TSIZE = 2048;
    std::vector<int16_t> slot{std::vector<int16_t>(TSIZE, -1)};
    const std::vector<std::string>* names{};

    static size_t key(size_t len, char first) { return ((len << 5) ^ static_cast<unsigned char>(first)) & (TSIZE - 1); }

    explicit FastMatcher(const std::vector<std::string>& n) : names(&n)
    {
        for (size_t i = 0; i < n.size(); ++i)
        {
            const size_t k = key(n[i].size(), n[i][0]);
            if (slot[k] != -1) { std::fprintf(stderr, "matcher collision, widen the table\n"); std::abort(); }
            slot[k] = static_cast<int16_t>(i);
        }
    }

    inline int find(const char* p, size_t len) const
    {
        if (len == 0) { return -1; }
        const int16_t cand = slot[key(len, p[0])];
        if (cand < 0) { return -1; }
        const std::string& nm = (*names)[static_cast<size_t>(cand)];
        if (nm.size() != len || std::memcmp(nm.data(), p, len) != 0) { return -1; }
        return cand;
    }
};

size_t runSimdFast(std::string_view buf, const FastMatcher& fm, size_t nFields)
{
    std::vector<uint32_t> pos;
    pos.reserve(256);
    std::vector<OffsetPair> out(nFields);
    size_t checksum = 0;

    size_t s = buf.find(DELIM);
    if (s == std::string_view::npos) { return 0; }
    s += 1;
    size_t e = buf.find(DELIM, s);
    while (e != std::string_view::npos)
    {
        const char* rec = buf.data() + s;
        const size_t len = e - s;
        if (std::string_view(rec, len).find_first_not_of(" \t\r\n") != std::string_view::npos)
        {
            scanStructurals(rec, len, pos);
            const auto ts = static_cast<uint32_t>(s);
            std::fill(out.begin(), out.end(), OffsetPair{ts, ts});
            size_t found = 0;
            for (size_t k = 2; k + 3 < pos.size() && found < nFields; k += 4)
            {
                const uint32_t ltOpen = pos[k], gtOpen = pos[k + 1], ltClose = pos[k + 2];
                if (rec[ltOpen + 1] == '/') { break; }
                const int fi = fm.find(rec + ltOpen + 1, gtOpen - ltOpen - 1);
                if (fi < 0) { continue; }
                auto& slot = out[static_cast<size_t>(fi)];
                if (slot.first != ts || slot.second != ts) { continue; }
                slot = {ts + gtOpen + 1, ts + ltClose};
                ++found;
            }
            emit(out, checksum);
        }
        s = e + 1;
        e = buf.find(DELIM, s);
    }
    return checksum;
}
} // namespace

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "Bimbo_190MB.xml";
    const int reps = (argc > 2) ? std::atoi(argv[2]) : 9;

    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!data.empty() && data.back() != DELIM) { data.push_back(DELIM); }
    data.insert(data.begin(), DELIM);

    FieldMap fm;
    for (uint32_t i = 0; i < kFields.size(); ++i) { fm.emplace(kFields[i], i); }

    const double mb = data.size() / 1048576.0;
    std::fprintf(stderr, "input %.1f MB, %d reps\n", mb, reps);

    const FastMatcher fast(kFields);

    size_t sa = 0, sb = 0, sc = 0;
    std::vector<double> ta, tb, tc;
    for (int r = 0; r < reps; ++r)
    {
        // Rotate the order of the three variants so none is systematically favoured.
        for (int k = 0; k < 3; ++k)
        {
            switch ((k + r) % 3)
            {
                case 0: {
                    auto t0 = std::chrono::steady_clock::now();
                    sa += runRapid(data, fm, kFields.size());
                    ta.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                    break;
                }
                case 1: {
                    auto t0 = std::chrono::steady_clock::now();
                    sb += runSimd(data, fm, kFields.size());
                    tb.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                    break;
                }
                default: {
                    auto t0 = std::chrono::steady_clock::now();
                    sc += runSimdFast(data, fast, kFields.size());
                    tc.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                    break;
                }
            }
        }
    }

    std::sort(ta.begin(), ta.end());
    std::sort(tb.begin(), tb.end());
    std::sort(tc.begin(), tc.end());
    std::printf("rapidxml           min %.4f s  (%6.1f MB/s)  median %.4f   1.00x\n",
                ta.front(), mb / ta.front(), ta[ta.size() / 2]);
    std::printf("simd               min %.4f s  (%6.1f MB/s)  median %.4f   %.2fx\n",
                tb.front(), mb / tb.front(), tb[tb.size() / 2], ta.front() / tb.front());
    std::printf("simd + fast names  min %.4f s  (%6.1f MB/s)  median %.4f   %.2fx\n",
                tc.front(), mb / tc.front(), tc[tc.size() / 2], ta.front() / tc.front());
    if (sa != sb || sa != sc)
    {
        std::printf("*** CHECKSUM MISMATCH: rapid=%zu simd=%zu fast=%zu -- NOT comparable\n", sa, sb, sc);
        return 2;
    }
    std::printf("all three checksums agree: %zu\n", sa);
    return 0;
}
