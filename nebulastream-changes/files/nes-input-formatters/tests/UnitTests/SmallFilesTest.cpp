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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Configuration/WorkerConfiguration.hpp>
#include <DataTypes/Schema.hpp>
#include <Identifiers/Identifiers.hpp>
#include <Interface/BufferRef/LowerSchemaProvider.hpp>
#include <Runtime/BufferManager.hpp>
#include <Runtime/TupleBuffer.hpp>
#include <Sources/SourceCatalog.hpp>
#include <Sources/SourceHandle.hpp>
#include <Sources/SourceReturnType.hpp>
#include <Util/Logger/LogLevel.hpp>
#include <Util/Logger/Logger.hpp>
#include <Util/Logger/impl/NesLogger.hpp>
#include <gtest/gtest.h>
#include <BaseUnitTest.hpp>
#include <ErrorHandling.hpp>
#include <FileUtil.hpp>
#include <InputFormatterTestUtil.hpp>
#include <InputFormatterValidationProvider.hpp>
#include <TestTaskQueue.hpp>

namespace
{
std::vector<size_t> getVarSizedFieldOffsets(const NES::Schema& schema)
{
    size_t priorFieldOffset = 0;
    std::vector<size_t> varSizedFieldOffsets;
    for (const auto& field : schema)
    {
        if (field.dataType.isType(NES::DataType::Type::VARSIZED))
        {
            varSizedFieldOffsets.emplace_back(priorFieldOffset);
        }
        priorFieldOffset += field.dataType.getSizeInBytesWithNull();
    }
    return varSizedFieldOffsets;
}
}

/// NOLINTBEGIN(readability-magic-numbers)
namespace NES
{
class SmallFilesTest : public Testing::BaseUnitTest
{
    struct TestFile
    {
        std::filesystem::path fileName;
        std::vector<InputFormatterTestUtil::TestDataTypes> schemaFieldTypes;
        std::vector<std::string> schemaFieldNames;
    };

    using enum InputFormatterTestUtil::TestDataTypes;
    std::unordered_map<std::string, TestFile> testFileMap{
        {"TwoIntegerColumns",
         TestFile{.fileName = "TwoIntegerColumns", .schemaFieldTypes = {INT32, INT32}, .schemaFieldNames = {"id", "value"}}},
        {"Bimbo", /// https://github.com/cwida/public_bi_benchmark/blob/master/benchmark/Bimbo/
         TestFile{
             .fileName = "Bimbo",
             .schemaFieldTypes = {INT16, INT16, INT32, INT16, FLOAT64, INT32, INT16, INT32, INT16, INT16, FLOAT64, INT16},
             .schemaFieldNames
             = {"Agencia_ID",
                "Canal_ID",
                "Cliente_ID",
                "Demanda_uni_equil",
                "Dev_proxima",
                "Dev_uni_proxima",
                "Number of Records",
                "Producto_ID",
                "Ruta_SAK",
                "Semana",
                "Venta_hoy",
                "Venta_uni_hoy"}}},
        {"Food", /// https://github.com/cwida/public_bi_benchmark/blob/master/benchmark/Food/
         TestFile{
             .fileName = "Food",
             .schemaFieldTypes = {INT16, INT32, VARSIZED, VARSIZED, INT16, FLOAT64},
             .schemaFieldNames = {"Number of Records", "activity_sec", "application", "device", "subscribers", "volume_total_bytes"}}},
        /// The XML variants below carry the same values as TwoIntegerColumns and can therefore
        /// reuse that file's independently verified expected output.
        {"TwoIntegerColumns_InlineComments", /// comments that contain no tuple delimiter
         TestFile{.fileName = "TwoIntegerColumns_InlineComments", .schemaFieldTypes = {INT32, INT32}, .schemaFieldNames = {"id", "value"}}},
        {"TwoIntegerColumns_MultilineComments", /// comments that contain a tuple delimiter
         TestFile{
             .fileName = "TwoIntegerColumns_MultilineComments", .schemaFieldTypes = {INT32, INT32}, .schemaFieldNames = {"id", "value"}}},
        {"TwoIntegerColumns_MultilineComments_RS", /// multiline comments; records separated by ASCII RS (0x1E)
         TestFile{
             .fileName = "TwoIntegerColumns_MultilineComments_RS",
             .schemaFieldTypes = {INT32, INT32},
             .schemaFieldNames = {"id", "value"}}},
        {"TwoIntegerColumns_DuplicateFields", /// repeated <id> must not mask a missing schema field
         TestFile{
             .fileName = "TwoIntegerColumns_DuplicateFields", .schemaFieldTypes = {INT32, INT32}, .schemaFieldNames = {"id", "value"}}},
        {"SDC_Metrics", /// generated; mirrors the IEEE 11073 SDC EpisodicMetricReport shape
         /// Exercises XML path addressing: a root attribute, element text and attributes on nested
         /// descendants (up to 4 levels), all with namespace prefixes. The CSV twin holds the same
         /// values positionally, so the expected .nes is produced by an independent parser.
         TestFile{
             .fileName = "SDC_Metrics",
             .schemaFieldTypes = {INT32, VARSIZED, VARSIZED, FLOAT64, INT64, VARSIZED},
             .schemaFieldNames
             = {"@MdibVersion",
                "msg:ReportPart/msg:SourceMds",
                "msg:ReportPart/msg:MetricState@DescriptorHandle",
                "msg:ReportPart/msg:MetricState/dom:MetricValue@Value",
                "msg:ReportPart/msg:MetricState/dom:MetricValue@DeterminationTime",
                "msg:ReportPart/msg:MetricState/dom:MetricValue/dom:MetricQuality@Validity"}}},
        {"Spacecraft_Telemetry", /// generated
         TestFile{
             .fileName = "Spacecraft_Telemetry",
             .schemaFieldTypes = {INT32, UINT32, BOOLEAN, CHAR, VARSIZED, FLOAT32, FLOAT64},
             .schemaFieldNames
             = {"temperature_delta",
                "power_level",
                "is_sunlit",
                "status_code",
                "operation_state",
                "radiation_level",
                "orbital_velocity"}}}};

    SourceCatalog sourceCatalog;

public:
    static void SetUpTestCase()
    {
        Logger::setupLogging("InputFormatterTest.log", LogLevel::LOG_DEBUG);
        NES_INFO("Setup InputFormatterTest test class.");
    }

    void SetUp() override { BaseUnitTest::SetUp(); }

    void TearDown() override { BaseUnitTest::TearDown(); }

    static bool writeBinaryToFile(const std::span<const std::ostream::char_type> data, const std::filesystem::path& filepath, bool append)
    {
        if (const auto parentPath = filepath.parent_path(); !parentPath.empty())
        {
            create_directories(parentPath);
        }

        std::ios_base::openmode openMode = std::ios::binary;
        openMode |= append ? std::ios::app : std::ios::trunc;

        std::ofstream file(filepath, openMode);
        if (not file)
        {
            throw InvalidConfigParameter("Could not open file: {}", filepath.string());
        }

        file.write(data.data(), static_cast<std::streamsize>(data.size_bytes()));

        return true;
    }

    struct TestConfig
    {
        std::string testFileName;
        std::string formatterType;
        std::string fileEnding;
        bool hasSpanningTuples;
        size_t numberOfIterations;
        size_t numberOfThreads;
        size_t sizeOfRawBuffers;
        bool isCompiled;
        std::string tupleDelimiter{"\n"};
        /// Reuse another fixture's expected output when multiple encodings contain identical tuples.
        std::string expectedFileName{};
    };

    struct SetupResult
    {
        Schema schema;
        MemoryLayoutType memoryLayoutType;
        size_t sizeOfFormattedBuffers;
        size_t numberOfExpectedRawBuffers;
        size_t numberOfRequiredFormattedBuffers;
        std::string currentTestFileName;
        std::string expectedFileName;
        std::string currentTestFilePath;
    };

    size_t getNumberOfExpectedBuffers(
        const TestConfig& testConfig, const std::filesystem::path& testFilePath, USED_IN_DEBUG const size_t sizeOfSchemaInBytes) const
    {
        const auto sizeOfFormattedBuffers = WorkerConfiguration().defaultQueryExecution.operatorBufferSize.getValue();
        PRECONDITION(
            sizeOfFormattedBuffers >= sizeOfSchemaInBytes, "The formatted buffer must be large enough to hold at least one tuple.");

        const auto fileSizeInBytes = std::filesystem::file_size(testFilePath);
        const auto numberOfExpectedRawBuffers
            = (fileSizeInBytes / testConfig.sizeOfRawBuffers) + static_cast<uint64_t>(fileSizeInBytes % testConfig.sizeOfRawBuffers != 0);
        return numberOfExpectedRawBuffers;
    }

    SetupResult setupTest(const TestConfig& testConfig, InputFormatterTestUtil::ThreadSafeVector<TupleBuffer>& rawBuffers)
    {
        const auto currentTestFile = testFileMap.at(testConfig.testFileName);
        const auto schema = InputFormatterTestUtil::createSchema(currentTestFile.schemaFieldTypes, currentTestFile.schemaFieldNames);

        const auto testDirPath = std::filesystem::path(INPUT_FORMATTER_TEST_DATA) / testConfig.fileEnding;
        const auto testFilePath = [](const TestFile& currentTestFile, const std::filesystem::path& testDirPath, std::string_view fileEnding)
        {
            if (const auto testFilePath = findFileByName(currentTestFile.fileName, testDirPath))
            {
                return testFilePath.value();
            }
            throw InvalidConfigParameter(
                "Could not find file test file: {}.<file_ending_of_{}>", testDirPath / currentTestFile.fileName, fileEnding);
        }(currentTestFile, testDirPath, testConfig.fileEnding);

        const auto sizeOfFormattedBuffers = WorkerConfiguration().defaultQueryExecution.operatorBufferSize.getValue();
        const auto numberOfExpectedRawBuffers = getNumberOfExpectedBuffers(testConfig, testFilePath, schema.getSizeOfSchemaInBytes());
        rawBuffers.reserve(numberOfExpectedRawBuffers);

        /// Create file source, start it using the emit function, and wait for the file source to fill the result buffer vector
        const auto numberOfRequiredSourceBuffers = static_cast<uint16_t>(numberOfExpectedRawBuffers + 1);
        std::shared_ptr<BufferManager> sourceBufferPool = BufferManager::create(testConfig.sizeOfRawBuffers, numberOfRequiredSourceBuffers);

        /// TODO #774: Sources sometimes need an extra buffer (reason currently unknown)
        const auto [backpressureController, fileSource] = InputFormatterTestUtil::createFileSource(
            sourceCatalog, testFilePath, schema, std::move(sourceBufferPool), numberOfRequiredSourceBuffers);
        fileSource->start(InputFormatterTestUtil::getEmitFunction(rawBuffers));
        rawBuffers.waitForSize(numberOfExpectedRawBuffers);
        INVARIANT(
            fileSource->tryStop(std::chrono::milliseconds(1000)) == SourceReturnType::TryStopResult::SUCCESS, "Failed to stop source.");
        INVARIANT(
            numberOfExpectedRawBuffers == rawBuffers.size(),
            "Expected to have {} raw buffers, but got: {}",
            numberOfExpectedRawBuffers,
            rawBuffers.size());

        /// We assume that we don't need more than two times the number of buffers to represent the formatted data than we need to represent the raw data
        const auto numberOfRequiredFormattedBuffers = static_cast<uint32_t>((rawBuffers.size() + 1) * 2);

        return SetupResult{
            .schema = schema,
            .memoryLayoutType = MemoryLayoutType::ROW_LAYOUT,
            .sizeOfFormattedBuffers = sizeOfFormattedBuffers,
            .numberOfExpectedRawBuffers = numberOfExpectedRawBuffers,
            .numberOfRequiredFormattedBuffers = numberOfRequiredFormattedBuffers,
            .currentTestFileName = currentTestFile.fileName,
            .expectedFileName = testConfig.expectedFileName.empty() ? currentTestFile.fileName.string() : testConfig.expectedFileName,
            .currentTestFilePath = testFilePath};
    }

    template <bool WriteExpectedResultsFile>
    bool compareResults(
        const std::vector<std::vector<TupleBuffer>>& resultBuffers,
        const SetupResult& setupResult,
        const std::vector<size_t>& varSizedFieldOffsets,
        BufferManager& testBufferManager)
    {
        /// Combine results and sort them using (ascending on sequence-/chunknumbers)
        auto combinedThreadResults = std::ranges::views::join(resultBuffers);
        std::vector<TupleBuffer> resultBufferVec(combinedThreadResults.begin(), combinedThreadResults.end());
        std::ranges::sort(
            resultBufferVec,
            [](const TupleBuffer& left, const TupleBuffer& right)
            {
                if (left.getSequenceNumber() == right.getSequenceNumber())
                {
                    return left.getChunkNumber() < right.getChunkNumber();
                }
                return left.getSequenceNumber() < right.getSequenceNumber();
            });


        /// Load expected results and compare to actual results
        if constexpr (WriteExpectedResultsFile)
        {
            const auto tmpExpectedResultsPath
                = std::filesystem::path(INPUT_FORMATTER_TMP_RESULT_DATA) / std::format("Expected/{}.nes", setupResult.currentTestFileName);
            writeTupleBuffersToFile(resultBufferVec, setupResult.schema, tmpExpectedResultsPath, varSizedFieldOffsets);
        }
        const auto expectedResultsPath
            = std::filesystem::path(INPUT_FORMATTER_TEST_DATA) / std::format("Expected/{}.nes", setupResult.expectedFileName);
        auto expectedBuffers = loadTupleBuffersFromFile(testBufferManager, setupResult.schema, expectedResultsPath, varSizedFieldOffsets);
        return InputFormatterTestUtil::compareTestTupleBuffersOrderSensitive(resultBufferVec, expectedBuffers, setupResult.schema);
    }

    template <bool WriteExpectedResultsFile = false>
    void runTest(const TestConfig& testConfig)
    {
        /// Create vector for result buffers and create emit function to collect buffers from source
        InputFormatterTestUtil::ThreadSafeVector<TupleBuffer> rawBuffers;

        const auto setupResult = setupTest(testConfig, rawBuffers);

        const auto varSizedFieldOffsets = getVarSizedFieldOffsets(setupResult.schema);
        for (size_t i = 0; i < testConfig.numberOfIterations; ++i)
        {
            /// Prepare TestTaskQueue for processing the input formatter tasks
            auto testBufferManager
                = BufferManager::create(setupResult.sizeOfFormattedBuffers, setupResult.numberOfRequiredFormattedBuffers);

            /// Create compiled pipeline stage containing InputFormatter and EmitOperator(emits formatted buffers into 'resultBuffers')

            auto inputFormatterConfig = std::unordered_map<std::string, std::string>{{"tuple_delimiter", testConfig.tupleDelimiter}};
            if (testConfig.formatterType == "CSV")
            {
                inputFormatterConfig.emplace("field_delimiter", "|");
            }
            const auto parserConfiguration
                = InputFormatterValidationProvider::provide(testConfig.formatterType, std::move(inputFormatterConfig)).value();
            auto testStage = InputFormatterTestUtil::createInputFormatter(
                parserConfiguration,
                setupResult.schema,
                setupResult.memoryLayoutType,
                setupResult.sizeOfFormattedBuffers,
                testConfig.isCompiled);

            auto resultBuffers = std::make_shared<std::vector<std::vector<TupleBuffer>>>(testConfig.numberOfThreads);
            std::vector<TestPipelineTask> pipelineTasks;
            pipelineTasks.reserve(setupResult.numberOfExpectedRawBuffers);
            rawBuffers.modifyBuffer(
                [&pipelineTasks, &testStage](const auto& buffers)
                {
                    std::ranges::for_each(
                        buffers, [&pipelineTasks, &testStage](const auto& rawBuffer) { pipelineTasks.emplace_back(rawBuffer, testStage); });
                });

            /// Create test task queue and process input formatter tasks
            auto taskQueue
                = std::make_unique<MultiThreadedTestTaskQueue>(testConfig.numberOfThreads, pipelineTasks, testBufferManager, resultBuffers);
            taskQueue->startProcessing();
            taskQueue->waitForCompletion();

            /// Check results
            const auto isCorrectResult
                = compareResults<WriteExpectedResultsFile>(*resultBuffers, setupResult, varSizedFieldOffsets, *testBufferManager);
            ASSERT_TRUE(isCorrectResult);

            /// Cleanup
            resultBuffers->clear();
            NES_DEBUG("Destroyed result buffer");
        }
    }
};

TEST_F(SmallFilesTest, testTwoIntegerColumnsJSON)
{
    runTest(TestConfig{
        .testFileName = "TwoIntegerColumns",
        .formatterType = "JSON",
        .fileEnding = "JSON",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 256,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testBimboDataJSON)
{
    runTest(TestConfig{
        .testFileName = "Bimbo",
        .formatterType = "JSON",
        .fileEnding = "JSON",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 16,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testFoodDataJSON)
{
    runTest(TestConfig{
        .testFileName = "Food",
        .formatterType = "JSON",
        .fileEnding = "JSON",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 16,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testSpaceCraftTelemetryJSON)
{
    runTest(TestConfig{
        .testFileName = "Spacecraft_Telemetry",
        .formatterType = "JSON",
        .fileEnding = "JSON",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 16,
        .isCompiled = true});
}

/// Characterises where XML comments interact with chunk-local tuple-delimiter detection.
/// A comment that contains no tuple delimiter is invisible to framing: rapidxml skips it inside
/// the record, so this is expected to pass.
TEST_F(SmallFilesTest, testInlineCommentsXML)
{
    runTest(TestConfig{
        .testFileName = "TwoIntegerColumns_InlineComments",
        .formatterType = "XML",
        .fileEnding = "XML",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 128,
        .isCompiled = true});
}

/// A comment that CONTAINS the tuple delimiter demonstrates the limitation of newline compatibility
/// mode: the '\n' is legal XML content rather than a record boundary.
///
/// DISABLED because it does not merely fail -- the indexer splits the comment at the '\n' and hands
/// rapidxml an unterminated fragment ("<agencia><!-- this comment"), which throws on a worker
/// thread and takes the whole process down with it, aborting every other test in this binary.
/// Enable it once delimiter-in-comment input is either handled or rejected gracefully.
TEST_F(SmallFilesTest, DISABLED_testMultilineCommentsXML)
{
    runTest(TestConfig{
        .testFileName = "TwoIntegerColumns_MultilineComments",
        .formatterType = "XML",
        .fileEnding = "XML",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 128,
        .isCompiled = true});
}

/// The same logical tuples with ASCII Record Separator (0x1E) between XML documents. Newlines stay
/// inside comments and are therefore passed to rapidxml as record content. Sixteen-byte source
/// buffers force records and comment markers to span many independently processed chunks.
TEST_F(SmallFilesTest, testMultilineCommentsWithRecordSeparatorXML)
{
    runTest(TestConfig{
        .testFileName = "TwoIntegerColumns_MultilineComments_RS",
        .formatterType = "XML",
        .fileEnding = "XML",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 16,
        .isCompiled = true,
        .tupleDelimiter = std::string(1, '\x1E'),
        .expectedFileName = "TwoIntegerColumns"});
}

/// A repeated element maps to one schema field and uses its first occurrence. Previously each
/// occurrence incremented the completeness counter, so two <id> elements could stop the scan before
/// <value> and make an incomplete offset table look complete.
TEST_F(SmallFilesTest, testDuplicateFieldsDoNotMaskSchemaFieldsXML)
{
    runTest(TestConfig{
        .testFileName = "TwoIntegerColumns_DuplicateFields",
        .formatterType = "XML",
        .fileEnding = "XML",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 32,
        .isCompiled = true,
        .expectedFileName = "TwoIntegerColumns"});
}

/// testdata/Expected/SDC_Metrics.nes was produced from this CSV twin by the independent CSV parser
/// (via runTest<true>), so it is not derived from the XML path-resolution code under test.
TEST_F(SmallFilesTest, testSDCMetricsCSV)
{
    runTest(TestConfig{
        .testFileName = "SDC_Metrics",
        .formatterType = "CSV",
        .fileEnding = "CSV",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 16,
        .isCompiled = true});
}

/// The actual attribute/nesting test: same values, addressed via XML paths, compared against the
/// expectation the CSV parser produced. Raw buffers are far smaller than one record, so most
/// records are also assembled through the spanning path.
TEST_F(SmallFilesTest, testSDCMetricsXML)
{
    runTest(TestConfig{
        .testFileName = "SDC_Metrics",
        .formatterType = "XML",
        .fileEnding = "XML",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 16,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testTwoIntegerColumnsXML)
{
    runTest(TestConfig{
        .testFileName = "TwoIntegerColumns",
        .formatterType = "XML",
        .fileEnding = "XML",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 128,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testBimboDataXML)
{
    runTest(TestConfig{
        .testFileName = "Bimbo",
        .formatterType = "XML",
        .fileEnding = "XML",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 512,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testFoodDataXML)
{
    runTest(TestConfig{
        .testFileName = "Food",
        .formatterType = "XML",
        .fileEnding = "XML",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 22,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testSpaceCraftTelemetryXML)
{
    runTest(TestConfig{
        .testFileName = "Spacecraft_Telemetry",
        .formatterType = "XML",
        .fileEnding = "XML",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 16,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testTwoIntegerColumns)
{
    runTest(TestConfig{
        .testFileName = "TwoIntegerColumns",
        .formatterType = "CSV",
        .fileEnding = "CSV",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 1,
        .sizeOfRawBuffers = 63,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testBimboData)
{
    runTest(TestConfig{
        .testFileName = "Bimbo",
        .formatterType = "CSV",
        .fileEnding = "CSV",
        .hasSpanningTuples = true,
        .numberOfIterations = 1,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 128,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testFoodData)
{
    runTest(TestConfig{
        .testFileName = "Food",
        .formatterType = "CSV",
        .fileEnding = "CSV",
        .hasSpanningTuples = true,
        .numberOfIterations = 10,
        .numberOfThreads = 8,
        .sizeOfRawBuffers = 2,
        .isCompiled = true});
}

TEST_F(SmallFilesTest, testSpaceCraftTelemetryData)
{
    runTest(
        {.testFileName = "Spacecraft_Telemetry",
         .formatterType = "CSV",
         .fileEnding = "CSV",
         .hasSpanningTuples = true,
         .numberOfIterations = 10,
         .numberOfThreads = 8,
         .sizeOfRawBuffers = 2,
         .isCompiled = true});
}

}

/// NOLINTEND(readability-magic-numbers)
