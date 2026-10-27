// Microbenchmark: record-local XML indexing with a parse copy (current) vs. terminating in
// place in the raw buffer and restoring the delimiter afterwards (proposed).
//
// Both variants do the *same* work otherwise: parse one delimiter-bounded record with rapidxml in
// non-destructive mode, then resolve every schema field to a (start,end) offset pair relative to
// the start of the raw buffer. Only the buffer-preparation strategy differs.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

// Resolve all schema fields of one already-parsed record. Shared by both variants so that the
// measured difference is exclusively buffer preparation.
inline void
resolveFields(rapidxml::xml_document<>& doc, const char* base, uint32_t tupleStart, const FieldMap& fm,
              std::vector<OffsetPair>& out, size_t& checksum)
{
    rapidxml::xml_node<>* root = doc.first_node();
    if (root == nullptr) { return; }
    std::fill(out.begin(), out.end(), OffsetPair{tupleStart, tupleStart});
    size_t found = 0;
    for (auto* n = root->first_node(); n != nullptr && found < out.size(); n = n->next_sibling())
    {
        auto it = fm.find(std::string_view(n->name(), n->name_size()));
        if (it == fm.end()) { continue; }
        auto& slot = out[it->second];
        if (slot.first != tupleStart || slot.second != tupleStart) { continue; }
        const auto s = static_cast<uint32_t>(n->value() - base);
        slot = {tupleStart + s, tupleStart + s + static_cast<uint32_t>(n->value_size())};
        ++found;
    }
    // Consume the result so nothing can be optimized away.
    for (const auto& p : out) { checksum += p.first + p.second; }
}

// ---- Variant A: copy each record into a reusable thread-local buffer (current implementation) ----
size_t runCopy(std::string_view buf, const FieldMap& fm, size_t nFields)
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
            resolveFields(doc, tl.data(), static_cast<uint32_t>(s), fm, out, checksum);
        }
        s = e + 1;
        e = buf.find(DELIM, s);
    }
    return checksum;
}

// ---- Variant B: terminate in place over the delimiter, parse, then restore it ----
size_t runInPlace(char* buf, size_t len, const FieldMap& fm, size_t nFields)
{
    rapidxml::xml_document<> doc;
    std::vector<OffsetPair> out(nFields);
    size_t checksum = 0;
    std::string_view view(buf, len);

    size_t s = view.find(DELIM);
    if (s == std::string_view::npos) { return 0; }
    s += 1;
    size_t e = view.find(DELIM, s);
    while (e != std::string_view::npos)
    {
        if (std::string_view(buf + s, e - s).find_first_not_of(" \t\r\n") != std::string_view::npos)
        {
            buf[e] = '\0';                 // the byte here is always DELIM, so no need to save it
            doc.clear();
            doc.parse<rapidxml::parse_non_destructive | rapidxml::parse_no_entity_translation>(buf + s);
            resolveFields(doc, buf + s, static_cast<uint32_t>(s), fm, out, checksum);
            buf[e] = DELIM;                // restore before anyone else can observe the buffer
        }
        s = e + 1;
        e = view.find(DELIM, s);
    }
    return checksum;
}
} // namespace

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "Bimbo_190MB.xml";
    const int reps = (argc > 2) ? std::atoi(argv[2]) : 5;
    const size_t chunk = (argc > 3) ? std::stoul(argv[3]) : 0; // 0 = whole file

    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (chunk > 0 && data.size() > chunk) { data.resize(chunk); }
    // Ensure the buffer both starts and ends on a delimiter, like a mid-stream raw buffer.
    if (!data.empty() && data.back() != DELIM) { data.push_back(DELIM); }
    data.insert(data.begin(), DELIM);

    FieldMap fm;
    for (uint32_t i = 0; i < kFields.size(); ++i) { fm.emplace(kFields[i], i); }

    std::fprintf(stderr, "input %.1f MB, %d reps\n", data.size() / 1048576.0, reps);

    size_t sinkA = 0, sinkB = 0;
    std::vector<double> ta, tb;
    for (int r = 0; r < reps; ++r)
    {
        // Alternate which variant runs first, so neither is systematically favoured by cache state.
        const bool copyFirst = (r % 2 == 0);
        for (int half = 0; half < 2; ++half)
        {
            if ((half == 0) == copyFirst)
            {
                auto t0 = std::chrono::steady_clock::now();
                sinkA += runCopy(data, fm, kFields.size());
                auto t1 = std::chrono::steady_clock::now();
                ta.push_back(std::chrono::duration<double>(t1 - t0).count());
            }
            else
            {
                auto t0 = std::chrono::steady_clock::now();
                sinkB += runInPlace(data.data(), data.size(), fm, kFields.size());
                auto t1 = std::chrono::steady_clock::now();
                tb.push_back(std::chrono::duration<double>(t1 - t0).count());
            }
        }
    }

    if (sinkA != sinkB)
    {
        std::fprintf(stderr, "MISMATCH: variants produced different offsets (%zu vs %zu)\n", sinkA, sinkB);
        return 2;
    }

    std::sort(ta.begin(), ta.end());
    std::sort(tb.begin(), tb.end());
    const double mb_ = data.size() / 1048576.0;
    const double amin = ta.front(), bmin = tb.front();
    const double amed = ta[ta.size() / 2], bmed = tb[tb.size() / 2];
    std::printf("copy     min %.4f s (%.1f MB/s)  median %.4f  max %.4f\n", amin, mb_ / amin, amed, ta.back());
    std::printf("in-place min %.4f s (%.1f MB/s)  median %.4f  max %.4f\n", bmin, mb_ / bmin, bmed, tb.back());
    std::printf("speedup on min:    %.4fx  (%+.2f%%)\n", amin / bmin, (amin / bmin - 1.0) * 100.0);
    std::printf("speedup on median: %.4fx  (%+.2f%%)\n", amed / bmed, (amed / bmed - 1.0) * 100.0);
    std::printf("checksums agree: %zu\n", sinkA);
    return 0;
}
