// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_clock_sources.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;
using atdecc::aem::DescriptorClockSource;

namespace {

/// The kinds of the CLOCK_SOURCE descriptors of the committed model @p name of testdata/.
auto kinds_of_model(char const* const name) -> std::vector<ClockSourceKind>
{
    std::ifstream in(std::filesystem::path{__FILE__}.parent_path() / "testdata" / name, std::ios::binary);
    std::vector<uint8_t> const blob{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto const storage = atdecc::aem::DescriptorStorage::create(blob);
    EXPECT_TRUE(storage.has_value());
    auto const kinds = storage.has_value() ? clock_source_kinds(*storage) : ClockSourceKinds{};
    return {kinds.begin(), kinds.end()};
}

/// A CLOCK_SOURCE of @p type located at descriptor type @p location.
constexpr auto source(uint16_t const type, uint16_t const location) -> DescriptorClockSource
{
    DescriptorClockSource described{};
    described.clock_source_type = type;
    described.clock_source_location_type = location;
    return described;
}

constexpr SavedClockSource DOMAIN_0{.clock_domain = 0, .clock_source = 1};
constexpr SavedClockSource DOMAIN_1{.clock_domain = 1, .clock_source = 12};

}  // namespace

TEST(clock_sources, a_clock_source_is_classified_by_its_type_and_where_it_is_located)
{
    using namespace atdecc::aem;
    EXPECT_TRUE(clock_source_kind_of(source(CLOCK_SOURCE_TYPE_INTERNAL, DESCRIPTOR_AUDIO_UNIT)) == ClockSourceKind::Internal);
    EXPECT_TRUE(
        clock_source_kind_of(source(CLOCK_SOURCE_TYPE_INPUT_STREAM, DESCRIPTOR_STREAM_INPUT)) == ClockSourceKind::InputStream);
    // Milan v1.3 7.5.2: gPTP is an EXTERNAL source located at a TIMING.
    EXPECT_TRUE(clock_source_kind_of(source(CLOCK_SOURCE_TYPE_EXTERNAL, DESCRIPTOR_TIMING)) == ClockSourceKind::Gptp);
    EXPECT_TRUE(clock_source_kind_of(source(CLOCK_SOURCE_TYPE_EXTERNAL, DESCRIPTOR_ENTITY)) == ClockSourceKind::External);
    EXPECT_TRUE(clock_source_kind_of(source(CLOCK_SOURCE_TYPE_EXTERNAL, DESCRIPTOR_JACK_INPUT)) == ClockSourceKind::External);
    // Located at a TIMING without being EXTERNAL, and a type the entity does not know.
    EXPECT_TRUE(clock_source_kind_of(source(CLOCK_SOURCE_TYPE_INTERNAL, DESCRIPTOR_TIMING)) == ClockSourceKind::Internal);
    EXPECT_TRUE(clock_source_kind_of(source(CLOCK_SOURCE_TYPE_MEDIA_CLOCK_STREAM, DESCRIPTOR_TIMING)) == ClockSourceKind::External);
}

TEST(clock_sources, the_shipped_models_clock_sources_are_what_their_json_says)
{
    // The blobs are written by the Python model compiler and read here: the
    // gPTP source is one only while both sides number TIMING alike.
    using enum ClockSourceKind;
    EXPECT_TRUE((kinds_of_model("entity_crf_generator.bin") == std::vector{Internal, Gptp, External, External}));
    EXPECT_TRUE((kinds_of_model("entity_tone_aaf.bin") == std::vector{Internal, InputStream}));
}

TEST(clock_sources, the_text_is_the_magic_and_one_line_per_clock_domain)
{
    std::array const sources{DOMAIN_0, DOMAIN_1};
    EXPECT_EQ(serialize_clock_sources(sources), std::string{"statusbar-clock-sources v1\ndomain 0 1\ndomain 1 12\n"});
    EXPECT_EQ(serialize_clock_sources({}), std::string{"statusbar-clock-sources v1\n"});
}

TEST(clock_sources, what_was_written_reads_back)
{
    std::array const sources{DOMAIN_0, DOMAIN_1};
    auto const parsed = parse_clock_sources(serialize_clock_sources(sources));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->size(), size_t{2});
    EXPECT_TRUE(parsed->at(0) == DOMAIN_0);
    EXPECT_TRUE(parsed->at(1) == DOMAIN_1);

    auto const none = parse_clock_sources("statusbar-clock-sources v1\n");
    EXPECT_TRUE(none.has_value());
    EXPECT_TRUE(none->empty());
}

TEST(clock_sources, blank_lines_comments_and_a_carriage_return_are_passed_over)
{
    auto const parsed = parse_clock_sources("# chosen sources\n\nstatusbar-clock-sources v1\r\n# the one domain\ndomain 0 1\r\n");
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->size(), size_t{1});
    EXPECT_TRUE(parsed->at(0) == DOMAIN_0);
}

TEST(clock_sources, a_text_that_is_not_the_file_is_refused_whole)
{
    constexpr std::string_view MAGIC{"statusbar-clock-sources v1\n"};
    for (std::string_view const body :
         {"domain 0\n",
          "domain 0 1 2\n",
          "domain x 1\n",
          "domain 0 -1\n",
          "domain 0 70000\n",
          "source 0 1\n",
          "domain 0 1\nrubbish\n"}) {
        EXPECT_FALSE(parse_clock_sources(std::string{MAGIC} + std::string{body}).has_value());
    }
    EXPECT_FALSE(parse_clock_sources("").has_value());
    EXPECT_FALSE(parse_clock_sources("domain 0 1\n").has_value());
    EXPECT_FALSE(parse_clock_sources("statusbar-clock-sources v2\ndomain 0 1\n").has_value());

    // One line more than there are clock domains kept.
    std::string many{MAGIC};
    for (size_t i = 0; i <= MAX_SAVED_CLOCK_SOURCES; ++i) {
        many += "domain " + std::to_string(i) + " 0\n";
    }
    auto const refused = parse_clock_sources(many);
    EXPECT_FALSE(refused.has_value());
    EXPECT_TRUE(refused.error() == std::errc::result_out_of_range);
}

TEST(clock_sources, a_file_that_is_not_there_is_no_saved_clock_source)
{
    auto const loaded = load_clock_sources("/no-such-directory-of-statusbar/clock-sources");
    EXPECT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->empty());
}

TEST_MAIN(statusbar_avb_entity, avb_entity_clock_sources_test)
