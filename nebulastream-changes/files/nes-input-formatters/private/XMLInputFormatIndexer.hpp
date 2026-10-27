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

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Configurations/Descriptor.hpp>
#include <DataTypes/DataType.hpp>
#include <Interface/BufferRef/TupleBufferRef.hpp>
#include <Interface/Record.hpp>
#include <Util/Strings.hpp>
#include <InputFormatIndexer.hpp>
#include <InputFormatterDescriptor.hpp>
#include <RawBufferIndex.hpp>
#include <RawValueParser.hpp>

namespace NES
{

/// Heterogeneous-lookup hash so unordered_map<std::string, ...>::find(std::string_view) compiles
/// without constructing a throwaway std::string per call.
struct XMLStringHash
{
    using is_transparent = void;

    [[nodiscard]] size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }

    [[nodiscard]] size_t operator()(const std::string& s) const noexcept { return std::hash<std::string_view>{}(s); }

    [[nodiscard]] size_t operator()(const char* s) const noexcept { return std::hash<std::string_view>{}(s); }
};

using XMLFieldNameToIndexMap = std::unordered_map<std::string, FieldIndex, XMLStringHash, std::equal_to<>>;

/// A schema field name, parsed into the location of its value within one record.
///
/// Syntax: "step/step/...@attribute", where any part may be omitted:
///   "Canal_ID"                              -> text of the direct child element <Canal_ID>
///   "@MdibVersion"                          -> attribute MdibVersion on the record's root element
///   "msg:MetricState@DescriptorHandle"      -> attribute on a direct child
///   "msg:MetricState/dom:MetricValue@Value" -> attribute on a nested descendant
///
/// A bare name (no '/' and no '@') keeps the original meaning exactly, so existing schemas are
/// unaffected. Steps are matched literally against rapidxml's node names, which include any
/// namespace prefix ("msg:MetricState") because rapidxml does not resolve namespaces -- that is
/// why prefixed paths need no separate handling.
///
/// Field names with punctuation must be backquoted in SQL (`AntlrSQL.g4` BACKQUOTED_IDENTIFIER);
/// unquoted identifiers are restricted to [A-Za-z_][A-Za-z0-9_]*.
struct XMLFieldPath
{
    /// Element names to descend from the record root. Empty means the value lives on the root itself.
    std::vector<std::string> steps;
    /// Attribute to read. Empty means read the element's text instead.
    std::string attribute;
};

struct ConfigParametersXMLInputFormatIndexer
{
    static inline const DescriptorConfig::ConfigParameter<char> TUPLE_DELIMITER{
        "tuple_delimiter",
        '\n',
        [](const std::unordered_map<std::string, std::string>& config) -> std::optional<char>
        {
            const auto it = config.find("tuple_delimiter");
            if (it == config.end())
            {
                return '\n';
            }
            const auto unescaped = unescapeSpecialCharacters(it->second);
            return (unescaped.size() == 1) ? std::optional<char>{unescaped.front()} : std::nullopt;
        }};

    static inline std::unordered_map<std::string, DescriptorConfig::ConfigParameterContainer> parameterMap
        = DescriptorConfig::createConfigParameterContainerMap(InputFormatterDescriptor::parameterMap, TUPLE_DELIMITER);
};

class XMLInputFormatIndexer : public InputFormatIndexer
{
    /// Passkey idiom to enforce going through create()
    struct Private
    {
        explicit Private() = default;
    };

public:
    static constexpr std::string_view NAME = "XML";
    /// Default XML root tag; carries over from the original prototype targeting the Bimbo dataset's <agencia>.
    /// Made into a constant so the field-finding loop has a single source of truth until a config knob is added.
    static constexpr std::string_view DEFAULT_ROOT_TAG = "agencia";

    explicit XMLInputFormatIndexer(
        Private, char tupleDelimiter, std::vector<Record::RecordFieldIdentifier> fieldNames, std::vector<DataType> fieldDataTypes);

    static std::unique_ptr<XMLInputFormatIndexer> create(const InputFormatterDescriptor& config, const TupleBufferRef& tupleBufferRef)
    {
        return std::make_unique<XMLInputFormatIndexer>(
            Private{},
            config.getFromConfig(ConfigParametersXMLInputFormatIndexer::TUPLE_DELIMITER),
            tupleBufferRef.getAllFieldNames(),
            tupleBufferRef.getAllDataTypes());
    }

    ~XMLInputFormatIndexer() override = default;

    [[nodiscard]] std::unique_ptr<RawBufferIndex> indexRawBuffer(std::string_view rawBuffer) const override;

    [[nodiscard]] std::string_view getTupleDelimitingBytes() const override { return tupleDelimiterStr; }

    [[nodiscard]] std::string_view getFieldDelimitingBytes() const override { return {}; }

    [[nodiscard]] QuotationType getQuotationType() const override { return QuotationType::NONE; }

    [[nodiscard]] const std::vector<std::string>& getNullValues() const override { return nullValues; }

    static DescriptorConfig::Config validateAndFormat(std::unordered_map<std::string, std::string> config);

protected:
    [[nodiscard]] std::ostream& toString(std::ostream& str) const override;

private:
    std::string rootTag;
    std::string tupleStart; ///< "<rootTag>"
    std::string tupleEnd; ///< "</rootTag>"
    char tupleDelimiter;
    std::string tupleDelimiterStr;
    std::vector<Record::RecordFieldIdentifier> fieldNames;
    std::vector<DataType> fieldDataTypes;
    /// Fast path: populated and used only when every field is a bare element name.
    XMLFieldNameToIndexMap fieldNameToIndexMap;
    /// General path: one entry per schema field, in schema order.
    std::vector<XMLFieldPath> fieldPaths;
    /// True when no field uses '/' or '@'. Then a single pass over the root's children resolves every
    /// field, as before. Kept as a distinct branch so the common (and already benchmarked) case does
    /// not regress into per-field tree walks.
    bool allFieldsAreBareElementNames{true};
    std::vector<std::string> nullValues;
};

}
