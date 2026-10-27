# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at

#    https://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Sequential SAX (Expat) baseline for the XML input formatter (thesis evaluation baseline).
# Optional: requires the system libexpat development package (e.g. `apt-get install libexpat1-dev`);
# skipped with a status message when absent so the main build never depends on it.
find_package(EXPAT QUIET)
find_package(Threads REQUIRED)
find_package(fmt CONFIG REQUIRED)
if (EXPAT_FOUND)
    add_executable(xml-sax-baseline SAXBaselineBenchmark.cpp)
    target_link_libraries(xml-sax-baseline PRIVATE EXPAT::EXPAT fmt::fmt Threads::Threads)
    message(STATUS "xml-sax-baseline: building with expat ${EXPAT_VERSION_STRING}")
else ()
    message(STATUS "xml-sax-baseline: libexpat not found, skipping baseline benchmark target (install libexpat1-dev)")
endif ()
