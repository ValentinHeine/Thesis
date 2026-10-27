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

#include <XMLInputFormatIndexer.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Configurations/Descriptor.hpp>
#include <DataTypes/Schema.hpp>
#include <Sources/SourceDescriptor.hpp>
#include <fmt/format.h>
#include <ErrorHandling.hpp>
#include <InputFormatIndexerRegistry.hpp>
#include <InputFormatter.hpp>
#include <InputFormatterValidationRegistry.hpp>
#include <RawBufferIndex.hpp>
#include <RawValueParser.hpp>
#include <XMLRawBufferIndex.hpp>
#include <rapidxml.hpp>

namespace
{

/// Schema column names may contain spaces ("Number of Records"); the corresponding XML tag cannot,
/// and the generators emit <Number_of_Records>. Applied per path step so nested paths behave the same.
std::string normalizePathStep(std::string_view raw)
{
    std::string normalized;
    normalized.reserve(raw.size());
    for (const char c : raw)
    {
        normalized.push_back(c == ' ' ? '_' : c);
    }
    return normalized;
}

/// Splits "step/step@attr" into its parts. See XMLFieldPath for the syntax.
NES::XMLFieldPath parseFieldPath(std::string_view rawName)
{
    NES::XMLFieldPath path;

    /// The attribute marker is the last '@' so that '@' inside an element name is not misread.
    std::string_view stepsPart = rawName;
    if (const auto atPos = rawName.rfind('@'); atPos != std::string_view::npos)
    {
        path.attribute = normalizePathStep(rawName.substr(atPos + 1));
        stepsPart = rawName.substr(0, atPos);
    }

    if (!stepsPart.empty())
    {
        size_t pos = 0;
        while (true)
        {
            const auto slash = stepsPart.find('/', pos);
            const auto end = (slash == std::string_view::npos) ? stepsPart.size() : slash;
            if (end > pos)
            {
                path.steps.emplace_back(normalizePathStep(stepsPart.substr(pos, end - pos)));
            }
            if (slash == std::string_view::npos)
            {
                break;
            }
            pos = slash + 1;
        }
    }
    return path;
}

void indexOneTuple(
    NES::XMLRawBufferIndex& rawBufferIndex,
    std::string_view tuple,
    const NES::FieldIndex startIdxOfTuple,
    const NES::XMLFieldNameToIndexMap& fieldMap,
    const std::vector<NES::XMLFieldPath>& fieldPaths,
    const bool allFieldsAreBareElementNames,
    const size_t numberOfFieldsInSchema,
    rapidxml::xml_document<>& doc)
{
    /// rapidxml needs a mutable, null-terminated buffer. The raw buffer handed to the indexer may be
    /// read-only memory (mmap / shared page), so we always copy out instead of mutating it in place.
    thread_local std::vector<char> tlParseBuffer;
    tlParseBuffer.resize(tuple.size() + 1);
    std::memcpy(tlParseBuffer.data(), tuple.data(), tuple.size());
    tlParseBuffer[tuple.size()] = '\0';
    char* const xmlBuffer = tlParseBuffer.data();

    doc.clear();
    try
    {
        doc.parse<rapidxml::parse_non_destructive | rapidxml::parse_no_entity_translation>(xmlBuffer);
    }
    catch (const rapidxml::parse_error& e)
    {
        throw NES::FormattingError("XML parse error: {}", e.what());
    }

    rapidxml::xml_node<>* root = doc.first_node();
    if (root == nullptr)
    {
        throw NES::FormattingError("XML contains no valid root node.");
    }

    /// Gather per-field (start, end) offsets in schema order. A field not present in the XML keeps
    /// this sentinel until the completeness check below rejects the record.
    const auto missingFieldSentinel = std::pair{startIdxOfTuple, startIdxOfTuple};
    std::vector<std::pair<NES::FieldIndex, NES::FieldIndex>> perFieldOffsets(numberOfFieldsInSchema, missingFieldSentinel);
    size_t numFieldsFound = 0;

    if (allFieldsAreBareElementNames)
    {
        /// Fast path: every field is a direct child element, so one pass over the root's children
        /// resolves all of them. This is the shape the throughput evaluation was measured on.
        for (rapidxml::xml_node<>* node = root->first_node(); node != nullptr && numFieldsFound < numberOfFieldsInSchema;
             node = node->next_sibling())
        {
            const std::string_view key(node->name(), node->name_size());
            const auto it = fieldMap.find(key);
            if (it == fieldMap.end())
            {
                continue;
            }

            /// A schema field denotes one value. Keep the first matching child, as the general
            /// path does, and do not let repeated elements inflate numFieldsFound and mask a
            /// different missing schema field.
            auto& fieldOffsets = perFieldOffsets[it->second];
            if (fieldOffsets != missingFieldSentinel)
            {
                continue;
            }

            const char* valuePtr = node->value();
            const size_t valueLen = node->value_size();

            const auto localStart = static_cast<NES::FieldIndex>(valuePtr - xmlBuffer);
            const auto localEnd = static_cast<NES::FieldIndex>(localStart + valueLen);

            fieldOffsets = {startIdxOfTuple + localStart, startIdxOfTuple + localEnd};
            ++numFieldsFound;
        }
    }
    else
    {
        /// General path: resolve each field by descending its element steps, then reading either the
        /// element's text or one of its attributes. Everything here is local to this one record, so
        /// nesting and attributes do not weaken the per-buffer statelessness the design relies on.
        for (size_t fieldIdx = 0; fieldIdx < fieldPaths.size(); ++fieldIdx)
        {
            const auto& path = fieldPaths[fieldIdx];

            rapidxml::xml_node<>* node = root;
            for (const auto& step : path.steps)
            {
                node = node->first_node(step.c_str(), step.size());
                if (node == nullptr)
                {
                    break;
                }
            }
            if (node == nullptr)
            {
                continue;
            }

            /// NOTE: when a step name repeats within a record (e.g. many <msg:MetricState> per SDC
            /// report) this takes the first match. Emitting one tuple per repeated element is a
            /// separate concern -- it changes the records-per-delimiter-region count, which the
            /// spanning-record path does not currently express.
            const char* valuePtr = nullptr;
            size_t valueLen = 0;
            if (path.attribute.empty())
            {
                valuePtr = node->value();
                valueLen = node->value_size();
            }
            else
            {
                const auto* attribute = node->first_attribute(path.attribute.c_str(), path.attribute.size());
                if (attribute == nullptr)
                {
                    continue;
                }
                valuePtr = attribute->value();
                valueLen = attribute->value_size();
            }

            const auto localStart = static_cast<NES::FieldIndex>(valuePtr - xmlBuffer);
            const auto localEnd = static_cast<NES::FieldIndex>(localStart + valueLen);

            perFieldOffsets[fieldIdx] = {startIdxOfTuple + localStart, startIdxOfTuple + localEnd};
            ++numFieldsFound;
        }
    }

    if (numFieldsFound != numberOfFieldsInSchema)
    {
        throw NES::FormattingError("Parsed {} XML fields but expected {}.", numFieldsFound, numberOfFieldsInSchema);
    }

    for (const auto& [s, e] : perFieldOffsets)
    {
        rawBufferIndex.emplaceFieldOffsetPair(s, e);
    }
}

}

namespace NES
{

XMLInputFormatIndexer::XMLInputFormatIndexer(
    Private, const char tupleDelimiterIn, std::vector<Record::RecordFieldIdentifier> fieldNamesIn, std::vector<DataType> fieldDataTypesIn)
    : rootTag(DEFAULT_ROOT_TAG)
    , tupleStart(fmt::format("<{}>", rootTag))
    , tupleEnd(fmt::format("</{}>", rootTag))
    , tupleDelimiter(tupleDelimiterIn)
    , tupleDelimiterStr(1, tupleDelimiter)
    , fieldNames(std::move(fieldNamesIn))
    , fieldDataTypes(std::move(fieldDataTypesIn))
    , nullValues({""})
{
    fieldPaths.reserve(fieldNames.size());
    for (size_t fieldIdx = 0; fieldIdx < fieldNames.size(); ++fieldIdx)
    {
        /// Strip the schema's qualifier prefix (e.g. "source$field" -> "field") so the lookup matches the XML tag name.
        std::string_view rawName = fieldNames[fieldIdx];
        if (const auto pos = rawName.find(Schema::ATTRIBUTE_NAME_SEPARATOR); pos != std::string::npos)
        {
            rawName = rawName.substr(pos + 1);
        }

        auto path = parseFieldPath(rawName);
        /// A bare element name is the original behaviour: exactly one step, reading element text.
        if (path.steps.size() == 1 && path.attribute.empty())
        {
            fieldNameToIndexMap.emplace(path.steps.front(), static_cast<FieldIndex>(fieldIdx));
        }
        else
        {
            allFieldsAreBareElementNames = false;
        }
        fieldPaths.push_back(std::move(path));
    }
}

std::unique_ptr<RawBufferIndex> XMLInputFormatIndexer::indexRawBuffer(std::string_view rawBuffer) const
{
    auto rawBufferIndex = std::make_unique<XMLRawBufferIndex>();
    const size_t numberOfFieldsInSchema = fieldNames.size();
    rawBufferIndex->startSetup(numberOfFieldsInSchema);

    /// Tuple boundaries are a configured single byte between concatenated records on the wire,
    /// exactly like the CSV indexer. A single byte is always fully visible in a chunk, whereas the
    /// multi-byte tags "<root>"/"</root>" can straddle a chunk boundary and be missed by a local scan.
    const auto offsetOfFirstTupleDelimiter = rawBuffer.find(tupleDelimiter);
    if (numberOfFieldsInSchema == 0 || offsetOfFirstTupleDelimiter == std::string_view::npos)
    {
        rawBufferIndex->markNoTupleDelimiters();
        return rawBufferIndex;
    }

    /// Index every complete record between two delimiters. The input contract guarantees each
    /// such range holds exactly one XML document; delimiter-wrapped whitespace holds none.
    rapidxml::xml_document<> doc;
    size_t startIdxOfNextTuple = offsetOfFirstTupleDelimiter + 1;
    size_t endIdxOfNextTuple = rawBuffer.find(tupleDelimiter, startIdxOfNextTuple);
    while (endIdxOfNextTuple != std::string_view::npos)
    {
        INVARIANT(startIdxOfNextTuple <= endIdxOfNextTuple, "The start index of a tuple cannot be larger than the end index.");
        const auto tupleView = rawBuffer.substr(startIdxOfNextTuple, endIdxOfNextTuple - startIdxOfNextTuple);
        if (tupleView.find_first_not_of(" \t\r\n") != std::string_view::npos)
        {
            indexOneTuple(
                *rawBufferIndex,
                tupleView,
                static_cast<FieldIndex>(startIdxOfNextTuple),
                fieldNameToIndexMap,
                fieldPaths,
                allFieldsAreBareElementNames,
                numberOfFieldsInSchema,
                doc);
        }
        startIdxOfNextTuple = endIdxOfNextTuple + 1;
        endIdxOfNextTuple = rawBuffer.find(tupleDelimiter, startIdxOfNextTuple);
    }

    const auto offsetOfLastTupleDelimiter = static_cast<FieldIndex>(startIdxOfNextTuple - 1);
    rawBufferIndex->markWithTupleDelimiters(static_cast<FieldIndex>(offsetOfFirstTupleDelimiter), offsetOfLastTupleDelimiter);
    return rawBufferIndex;
}

DescriptorConfig::Config XMLInputFormatIndexer::validateAndFormat(std::unordered_map<std::string, std::string> config)
{
    return DescriptorConfig::validateAndFormat<ConfigParametersXMLInputFormatIndexer>(std::move(config), NAME);
}

InputFormatIndexerRegistryReturnType RegisterXMLInputFormatIndexer(InputFormatIndexerRegistryArguments arguments)
{
    return arguments.createInputFormatterWithIndexer(
        XMLInputFormatIndexer::create(arguments.getInputFormatterConfig(), arguments.getInputMemoryProvider()));
}

InputFormatterValidationRegistryReturnType
InputFormatterValidationGeneratedRegistrar::RegisterXMLInputFormatterValidation(InputFormatterValidationRegistryArguments arguments)
{
    return XMLInputFormatIndexer::validateAndFormat(arguments.config);
}

std::ostream& XMLInputFormatIndexer::toString(std::ostream& str) const
{
    return str << fmt::format("XMLInputFormatIndexer(root={})", rootTag);
}
}
