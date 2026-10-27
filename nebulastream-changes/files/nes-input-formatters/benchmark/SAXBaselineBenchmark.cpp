/*
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        https://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/// Sequential SAX (Expat) baseline for the chunk-parallel XML input formatter (thesis baseline).
///
/// Models "what you would build without NebulaStream": one Expat parser instance per source,
/// each source parsed sequentially front-to-back, parallelism only ACROSS sources (one thread
/// per source). Extracts the same fields as XMLInputFormatIndexer (direct children of the record
/// element, matched by exact tag name), converts them to the same native types, and optionally
/// renders each tuple as a CSV line exactly like NES's CSVOutputFormatter + Checksum sink
/// (integers via std::to_string, floats via {:.6f} with trailing-zero stripping, ',' between
/// fields, '\n' after the tuple, checksum = byte sum, tuple count = newline count). A matching
/// count/checksum against a Bimbo_XML.test query is a cross-system correctness validation.
///
/// The input files are streams of concatenated record documents (no single root), so the stream
/// is wrapped in a synthetic root element before feeding it to Expat. Input must not contain an
/// XML declaration. Entities are expanded by Expat (the chunk indexer uses no-entity-translation;
/// the benchmark data contains no entities, so the outputs agree).

#include <expat.h>

#include <atomic>
#include <barrier>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

namespace
{

enum class FieldType : std::uint8_t
{
    Int16,
    Int32,
    Int64,
    UInt64,
    Float32,
    Float64,
};

struct FieldSpec
{
    std::string name;
    FieldType type;
};

struct Schema
{
    std::string recordTag;
    std::vector<FieldSpec> fields;
};

struct Options
{
    std::vector<std::string> files;
    Schema schema;
    std::size_t sources = 1;
    std::size_t runs = 1;
    std::size_t chunkBytes = std::size_t{64} * 1024;
    bool parseOnly = false; /// skip CSV rendering + checksum; count tuples and fold values into a sink
};

/// Heterogeneous string hash so the per-field tag lookup takes the Expat const char* without
/// constructing a temporary std::string per field per record (same trick as the chunk indexer).
struct TransparentStringHash
{
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(const std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
    [[nodiscard]] std::size_t operator()(const std::string& text) const noexcept { return std::hash<std::string_view>{}(text); }
    [[nodiscard]] std::size_t operator()(const char* text) const noexcept { return std::hash<std::string_view>{}(text); }
};

/// Replicates NES::Util::formatFloat (nes-common/src/Util/Strings.cpp): fixed 6 decimals via fmt,
/// trailing zeros stripped, at least one decimal digit kept ("1.000000" -> "1.0").
void appendFormattedFloat(std::string& out, const double value)
{
    std::string formatted = fmt::format("{:.6f}", value);
    const std::size_t decimalPos = formatted.find('.');
    if (decimalPos == std::string::npos)
    {
        out += formatted;
        return;
    }
    const std::size_t lastNonZero = formatted.find_last_not_of('0');
    if (lastNonZero == decimalPos)
    {
        out.append(formatted, 0, decimalPos + 2);
        return;
    }
    out.append(formatted, 0, lastNonZero + 1);
}

struct TypedValue
{
    std::int64_t asSigned = 0;
    std::uint64_t asUnsigned = 0;
    double asDouble = 0.0;
};

/// Per-source parser: Expat callbacks drive a small state machine over
/// wrapper(depth 1) -> record(depth 2) -> field(depth 3). Field text deeper than the direct
/// child level is ignored, mirroring rapidxml's node->value() semantics in the chunk indexer.
class SourceParser
{
public:
    SourceParser(const Schema& schema, const bool parseOnly) : schema(schema), parseOnly(parseOnly)
    {
        for (std::size_t i = 0; i < schema.fields.size(); ++i)
        {
            fieldIndexByName.emplace(schema.fields[i].name, i);
        }
        slots.resize(schema.fields.size());
        present.resize(schema.fields.size(), 0);
        typedRow.resize(schema.fields.size());
    }

    bool parseFile(const std::string& path, const std::size_t chunkBytes)
    {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (file == nullptr)
        {
            fmt::print(stderr, "cannot open {}\n", path);
            return false;
        }

        XML_Parser parser = XML_ParserCreate(nullptr);
        XML_SetUserData(parser, this);
        XML_SetElementHandler(parser, &SourceParser::onStartElement, &SourceParser::onEndElement);
        XML_SetCharacterDataHandler(parser, &SourceParser::onCharacterData);

        bool ok = XML_Parse(parser, "<s>", 3, XML_FALSE) == XML_STATUS_OK;
        std::vector<char> readBuffer(chunkBytes);
        while (ok)
        {
            const std::size_t bytesRead = std::fread(readBuffer.data(), 1, readBuffer.size(), file);
            if (bytesRead == 0)
            {
                break;
            }
            bytesFed += bytesRead;
            ok = XML_Parse(parser, readBuffer.data(), static_cast<int>(bytesRead), XML_FALSE) == XML_STATUS_OK;
        }
        if (ok)
        {
            ok = XML_Parse(parser, "</s>", 4, XML_TRUE) == XML_STATUS_OK;
        }
        if (!ok)
        {
            fmt::print(
                stderr,
                "XML parse error in {} at line {}: {}\n",
                path,
                XML_GetCurrentLineNumber(parser),
                XML_ErrorString(XML_GetErrorCode(parser)));
        }
        XML_ParserFree(parser);
        static_cast<void>(std::fclose(file));
        return ok;
    }

    std::uint64_t tuples = 0;
    std::uint64_t checksum = 0; /// byte sum over rendered CSV lines (NES Checksum::add semantics)
    std::uint64_t recordErrors = 0; /// records with missing schema fields (indexer would throw)
    std::uint64_t parseOnlySink = 0; /// folds converted values so parse-only mode cannot be optimized away
    std::uint64_t bytesFed = 0;

private:
    static void onStartElement(void* userData, const XML_Char* name, const XML_Char** /*attributes*/)
    {
        auto& self = *static_cast<SourceParser*>(userData);
        ++self.depth;
        if (self.depth == 2)
        {
            if (self.schema.recordTag == name)
            {
                self.inRecord = true;
                std::memset(self.present.data(), 0, self.present.size());
            }
        }
        else if (self.depth == 3 && self.inRecord)
        {
            const auto it = self.fieldIndexByName.find(name);
            self.currentField = (it != self.fieldIndexByName.end()) ? static_cast<std::ptrdiff_t>(it->second) : -1;
            self.fieldHasChildElement = false;
            if (self.currentField >= 0)
            {
                self.slots[static_cast<std::size_t>(self.currentField)].clear();
            }
        }
        else if (self.depth > 3 && self.currentField >= 0)
        {
            /// Nested element inside a field: the chunk indexer's node->value() only covers text
            /// before the first child, so stop collecting for this field.
            self.fieldHasChildElement = true;
        }
    }

    static void onCharacterData(void* userData, const XML_Char* text, const int length)
    {
        auto& self = *static_cast<SourceParser*>(userData);
        if (self.depth == 3 && self.currentField >= 0 && !self.fieldHasChildElement)
        {
            self.slots[static_cast<std::size_t>(self.currentField)].append(text, static_cast<std::size_t>(length));
        }
    }

    static void onEndElement(void* userData, const XML_Char* /*name*/)
    {
        auto& self = *static_cast<SourceParser*>(userData);
        if (self.depth == 3 && self.currentField >= 0)
        {
            self.present[static_cast<std::size_t>(self.currentField)] = 1;
            self.currentField = -1;
        }
        else if (self.depth == 2 && self.inRecord)
        {
            self.finishRecord();
            self.inRecord = false;
        }
        --self.depth;
    }

    void finishRecord()
    {
        for (const char fieldPresent : present)
        {
            if (fieldPresent == 0)
            {
                ++recordErrors;
                return;
            }
        }

        /// Convert raw field text to native values — the same text-to-native work the JIT'd
        /// NES pipeline performs per field when reading indexed offsets.
        for (std::size_t i = 0; i < schema.fields.size(); ++i)
        {
            const std::string& text = slots[i];
            TypedValue& value = typedRow[i];
            switch (schema.fields[i].type)
            {
                case FieldType::Int16:
                case FieldType::Int32:
                case FieldType::Int64: {
                    std::int64_t parsed = 0;
                    std::from_chars(text.data(), text.data() + text.size(), parsed);
                    value.asSigned = parsed;
                    break;
                }
                case FieldType::UInt64: {
                    std::uint64_t parsed = 0;
                    std::from_chars(text.data(), text.data() + text.size(), parsed);
                    value.asUnsigned = parsed;
                    break;
                }
                case FieldType::Float32:
                case FieldType::Float64: {
                    value.asDouble = std::strtod(text.c_str(), nullptr);
                    break;
                }
            }
        }
        ++tuples;

        if (parseOnly)
        {
            for (std::size_t i = 0; i < schema.fields.size(); ++i)
            {
                switch (schema.fields[i].type)
                {
                    case FieldType::Int16:
                    case FieldType::Int32:
                    case FieldType::Int64:
                        parseOnlySink ^= static_cast<std::uint64_t>(typedRow[i].asSigned);
                        break;
                    case FieldType::UInt64:
                        parseOnlySink ^= typedRow[i].asUnsigned;
                        break;
                    case FieldType::Float32:
                    case FieldType::Float64:
                        parseOnlySink ^= std::bit_cast<std::uint64_t>(typedRow[i].asDouble);
                        break;
                }
            }
            return;
        }

        /// Render the tuple as one CSV line exactly like NES's CSVOutputFormatter and fold it
        /// into the Checksum-sink accumulator (byte sum; one '\n' per tuple).
        lineBuffer.clear();
        for (std::size_t i = 0; i < schema.fields.size(); ++i)
        {
            switch (schema.fields[i].type)
            {
                case FieldType::Int16:
                case FieldType::Int32:
                case FieldType::Int64:
                    lineBuffer += std::to_string(typedRow[i].asSigned);
                    break;
                case FieldType::UInt64:
                    lineBuffer += std::to_string(typedRow[i].asUnsigned);
                    break;
                case FieldType::Float32:
                case FieldType::Float64:
                    appendFormattedFloat(lineBuffer, typedRow[i].asDouble);
                    break;
            }
            lineBuffer += (i + 1 == schema.fields.size()) ? '\n' : ',';
        }
        std::uint64_t lineSum = 0;
        for (const char character : lineBuffer)
        {
            lineSum += static_cast<unsigned char>(character);
        }
        checksum += lineSum;
    }

    const Schema& schema;
    const bool parseOnly;
    std::unordered_map<std::string, std::size_t, TransparentStringHash, std::equal_to<>> fieldIndexByName;

    int depth = 0;
    bool inRecord = false;
    bool fieldHasChildElement = false;
    std::ptrdiff_t currentField = -1;
    std::vector<std::string> slots;
    std::vector<char> present;
    std::vector<TypedValue> typedRow;
    std::string lineBuffer;
};

Schema bimboSchema()
{
    return Schema{
        .recordTag = "agencia",
        .fields = {
            {.name = "AGENCIA_ID", .type = FieldType::Int16},
            {.name = "CANAL_ID", .type = FieldType::Int16},
            {.name = "CLIENTE_ID", .type = FieldType::Int32},
            {.name = "DEMANDA_UNI_EQUIL", .type = FieldType::Int16},
            {.name = "DEV_PROXIMA", .type = FieldType::Float64},
            {.name = "DEV_UNI_PROXIMA", .type = FieldType::Int32},
            {.name = "NUMBER_OF_RECORDS", .type = FieldType::Int16},
            {.name = "PRODUCTO_ID", .type = FieldType::Int32},
            {.name = "RUTA_SAK", .type = FieldType::Int16},
            {.name = "SEMANA", .type = FieldType::Int16},
            {.name = "VENTA_HOY", .type = FieldType::Float64},
            {.name = "VENTA_UNI_HOY", .type = FieldType::Int16},
        }};
}

Schema twoIntSchema()
{
    return Schema{
        .recordTag = "agencia",
        .fields = {
            {.name = "id", .type = FieldType::Int64},
            {.name = "value", .type = FieldType::Int64},
        }};
}

bool parseFieldType(const std::string_view text, FieldType& out)
{
    if (text == "INT16") { out = FieldType::Int16; return true; }
    if (text == "INT32") { out = FieldType::Int32; return true; }
    if (text == "INT64") { out = FieldType::Int64; return true; }
    if (text == "UINT64") { out = FieldType::UInt64; return true; }
    if (text == "FLOAT32") { out = FieldType::Float32; return true; }
    if (text == "FLOAT64") { out = FieldType::Float64; return true; }
    return false;
}

void printUsage(const char* program)
{
    fmt::print(
        stderr,
        "Usage: {} --file <path> [--file <path> ...] (--preset bimbo|twoint | --record <tag> --field NAME:TYPE ...)\n"
        "          [--sources N] [--runs R] [--chunk-bytes B] [--parse-only]\n"
        "\n"
        "  --file        input file; with --sources N > file count, files are assigned round-robin\n"
        "  --preset      bimbo  = Bimbo_500MB.xml schema (record <agencia>, 12 uppercase fields)\n"
        "                twoint = TwoIntegerColumns.xml schema (record <agencia>, id/value INT64)\n"
        "  --record      record element tag for a custom schema\n"
        "  --field       schema field, e.g. VENTA_HOY:FLOAT64 (types: INT16|INT32|INT64|UINT64|FLOAT32|FLOAT64)\n"
        "  --sources     number of concurrent sources = threads, one Expat parser each (default 1)\n"
        "  --runs        repetitions; first run warms the page cache (default 1)\n"
        "  --chunk-bytes read/feed chunk size in bytes (default 65536)\n"
        "  --parse-only  skip CSV rendering + checksum (pure parse + convert throughput)\n",
        program);
}

bool parseArguments(const int argc, char** argv, Options& options)
{
    std::string presetName;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i];
        const auto nextValue = [&]() -> const char*
        {
            if (i + 1 >= argc)
            {
                fmt::print(stderr, "missing value for {}\n", argument);
                return nullptr;
            }
            return argv[++i];
        };

        if (argument == "--file")
        {
            const char* value = nextValue();
            if (value == nullptr) { return false; }
            options.files.emplace_back(value);
        }
        else if (argument == "--preset")
        {
            const char* value = nextValue();
            if (value == nullptr) { return false; }
            presetName = value;
        }
        else if (argument == "--record")
        {
            const char* value = nextValue();
            if (value == nullptr) { return false; }
            options.schema.recordTag = value;
        }
        else if (argument == "--field")
        {
            const char* value = nextValue();
            if (value == nullptr) { return false; }
            const std::string_view spec = value;
            const std::size_t colon = spec.rfind(':');
            FieldType type{};
            if (colon == std::string_view::npos || !parseFieldType(spec.substr(colon + 1), type))
            {
                fmt::print(stderr, "invalid --field spec: {}\n", spec);
                return false;
            }
            options.schema.fields.push_back({.name = std::string(spec.substr(0, colon)), .type = type});
        }
        else if (argument == "--sources")
        {
            const char* value = nextValue();
            if (value == nullptr) { return false; }
            options.sources = std::strtoull(value, nullptr, 10);
        }
        else if (argument == "--runs")
        {
            const char* value = nextValue();
            if (value == nullptr) { return false; }
            options.runs = std::strtoull(value, nullptr, 10);
        }
        else if (argument == "--chunk-bytes")
        {
            const char* value = nextValue();
            if (value == nullptr) { return false; }
            options.chunkBytes = std::strtoull(value, nullptr, 10);
        }
        else if (argument == "--parse-only")
        {
            options.parseOnly = true;
        }
        else
        {
            fmt::print(stderr, "unknown argument: {}\n", argument);
            return false;
        }
    }

    if (presetName == "bimbo")
    {
        options.schema = bimboSchema();
    }
    else if (presetName == "twoint")
    {
        options.schema = twoIntSchema();
    }
    else if (!presetName.empty())
    {
        fmt::print(stderr, "unknown preset: {}\n", presetName);
        return false;
    }

    if (options.files.empty() || options.schema.recordTag.empty() || options.schema.fields.empty() || options.sources == 0
        || options.runs == 0 || options.chunkBytes == 0)
    {
        return false;
    }
    return true;
}

}

int main(const int argc, char** argv)
{
    Options options;
    if (!parseArguments(argc, argv, options))
    {
        printUsage(argv[0]);
        return 1;
    }

    for (std::size_t run = 1; run <= options.runs; ++run)
    {
        std::vector<std::unique_ptr<SourceParser>> parsers;
        parsers.reserve(options.sources);
        for (std::size_t i = 0; i < options.sources; ++i)
        {
            parsers.emplace_back(std::make_unique<SourceParser>(options.schema, options.parseOnly));
        }

        std::atomic<bool> anyFailed{false};
        std::barrier startBarrier(static_cast<std::ptrdiff_t>(options.sources) + 1);
        std::vector<std::thread> threads;
        threads.reserve(options.sources);
        for (std::size_t i = 0; i < options.sources; ++i)
        {
            threads.emplace_back(
                [&, i]()
                {
                    const std::string& path = options.files[i % options.files.size()];
                    startBarrier.arrive_and_wait();
                    if (!parsers[i]->parseFile(path, options.chunkBytes))
                    {
                        anyFailed = true;
                    }
                });
        }

        startBarrier.arrive_and_wait();
        const auto startTime = std::chrono::steady_clock::now();
        for (auto& thread : threads)
        {
            thread.join();
        }
        const auto endTime = std::chrono::steady_clock::now();

        if (anyFailed)
        {
            return 1;
        }

        std::uint64_t totalTuples = 0;
        std::uint64_t totalChecksum = 0;
        std::uint64_t totalErrors = 0;
        std::uint64_t totalBytes = 0;
        std::uint64_t foldedSink = 0;
        for (const auto& parser : parsers)
        {
            totalTuples += parser->tuples;
            totalChecksum += parser->checksum;
            totalErrors += parser->recordErrors;
            totalBytes += parser->bytesFed;
            foldedSink ^= parser->parseOnlySink;
        }

        const double seconds = std::chrono::duration<double>(endTime - startTime).count();
        fmt::print(
            "run={} sources={} bytes={} tuples={} record_errors={} seconds={:.3f} mb_per_s={:.1f} tuples_per_s={:.0f}",
            run,
            options.sources,
            totalBytes,
            totalTuples,
            totalErrors,
            seconds,
            static_cast<double>(totalBytes) / 1.0e6 / seconds,
            static_cast<double>(totalTuples) / seconds);
        if (options.parseOnly)
        {
            fmt::print(" sink={:x}\n", foldedSink);
        }
        else
        {
            /// Same "<count> <checksum>" shape as a systest Checksum-sink expectation line.
            fmt::print(" systest_equivalent: {} {}\n", totalTuples, totalChecksum);
        }
    }
    return 0;
}
