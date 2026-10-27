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

#include <XMLRawBufferIndex.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include <function.hpp>
#include <static.hpp>

#include <DataTypes/DataTypesUtil.hpp>
#include <Interface/BufferRef/TupleBufferRef.hpp>
#include <Interface/Record.hpp>
#include <ErrorHandling.hpp>
#include <FieldOffsetRawBufferIndex.hpp>
#include <InputFormatter.hpp>
#include <RawBufferIndex.hpp>
#include <RawValueParser.hpp>
#include <val_arith.hpp>
#include <val_bool.hpp>
#include <val_concepts.hpp>
#include <val_ptr.hpp>

namespace NES
{

XMLRawBufferIndex::XMLRawBufferIndex()
{
    INVARIANT(
        static_cast<void*>(this) == static_cast<void*>(static_cast<RawBufferIndex*>(this)),
        "RawBufferIndex base subobject must lay out at offset 0 in XMLRawBufferIndex");
}

nautilus::val<bool>
XMLRawBufferIndex::hasNext(const nautilus::val<uint64_t>& tupleIdx, const nautilus::val<RawBufferIndex*>& rawBufferIndex) const
{
    nautilus::val<uint64_t> numTuples
        = readValueFromMemRef<size_t>(getMemberRef(rawBufferIndex, &XMLRawBufferIndex::totalNumberOfTuples));
    return tupleIdx < numTuples;
}

const FieldIndex* XMLRawBufferIndex::getIndexValuesProxy(const RawBufferIndex* rawBufferIndex)
{
    PRECONDITION(
        dynamic_cast<const XMLRawBufferIndex*>(rawBufferIndex) != nullptr, "rawBufferIndex must be a XMLRawBufferIndex");
    /// NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): type verified by PRECONDITION above.
    const auto* const fieldOffsets = static_cast<const XMLRawBufferIndex*>(rawBufferIndex);
    return fieldOffsets->indexValues.data();
}


Record XMLRawBufferIndex::readSpanningRecord(
    const std::vector<Record::RecordFieldIdentifier>& projections,
    const nautilus::val<int8_t*>& recordBufferPtr,
    const nautilus::val<uint64_t>& recordIndex,
    const InputFormatIndexer& indexer,
    nautilus::val<RawBufferIndex*> rawBufferIndex,
    const TupleBufferRef& bufferRef) const
{
    Record record;
    const auto indexBufferPtr = nautilus::invoke(getIndexValuesProxy, rawBufferIndex);
    const auto numberOfFields = nautilus::static_val{bufferRef.getAllDataTypes().size()};
    for (nautilus::static_val<uint64_t> i = 0; i < numberOfFields; ++i)
    {
        const auto fieldName = bufferRef.getAllFieldNames().at(i);
        const auto fieldDataType = bufferRef.getAllDataTypes().at(i);
        if (not includesField(projections, fieldName))
        {
            continue;
        }

        /// Each tuple stores 2 offsets per field (start, end); field i has start at (recordIndex * 2N) + 2i,
        /// end at (recordIndex * 2N) + 2i + 1, all in FieldIndex-sized units.
        const auto numPriorOffsets = recordIndex * nautilus::static_val(numberOfFields * 2);
        const auto fieldOffsetStartAddress = indexBufferPtr + (numPriorOffsets + i * nautilus::static_val<uint64_t>(2));
        const auto fieldOffsetEndAddress
            = indexBufferPtr + (numPriorOffsets + i * nautilus::static_val<uint64_t>(2) + nautilus::static_val<uint64_t>(1));
        const auto fieldOffsetStart = readValueFromMemRef<FieldIndex>(fieldOffsetStartAddress);
        const auto fieldOffsetEnd = readValueFromMemRef<FieldIndex>(fieldOffsetEndAddress);

        const auto fieldSize = fieldOffsetEnd - fieldOffsetStart;
        const auto fieldAddress = recordBufferPtr + fieldOffsetStart;
        parseRawValueIntoRecord(
            fieldDataType, record, fieldAddress, fieldSize, fieldName, indexer.getNullValues(), indexer.getQuotationType());
    }
    return record;
}

void XMLRawBufferIndex::startSetup(const size_t numberOfFieldsInSchema)
{
    this->numberOfFieldsInSchema = numberOfFieldsInSchema;
    this->numberOfOffsetsPerTuple = this->numberOfFieldsInSchema * 2;
    this->totalNumberOfTuples = 0;
}

void XMLRawBufferIndex::emplaceFieldOffsetPair(FieldIndex start, FieldIndex end)
{
    indexValues.emplace_back(start);
    indexValues.emplace_back(end);
}

void XMLRawBufferIndex::markNoTupleDelimiters()
{
    this->offsetOfFirstTuple = std::numeric_limits<FieldIndex>::max();
    this->offsetOfLastTuple = std::numeric_limits<FieldIndex>::max();
}

void XMLRawBufferIndex::markWithTupleDelimiters(const FieldIndex offsetToFirstTuple, const FieldIndex offsetToLastTuple)
{
    if (numberOfOffsetsPerTuple > 0)
    {
        const auto numberOfTuples = indexValues.size() / numberOfOffsetsPerTuple;
        const auto remainder = indexValues.size() % numberOfOffsetsPerTuple;
        INVARIANT(
            remainder == 0,
            "Number of indexes {} must be a multiple of number of offsets per tuple {} (remainder: {})",
            indexValues.size(),
            numberOfOffsetsPerTuple,
            remainder);
        this->totalNumberOfTuples = numberOfTuples;
    }
    this->offsetOfFirstTuple = offsetToFirstTuple;
    this->offsetOfLastTuple = offsetToLastTuple;
}
}
