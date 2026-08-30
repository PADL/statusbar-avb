// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_aem_model_store.hpp"

#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/nanoavb/nanoavb_aem_fingerprint.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace statusbar::nanoavb {

namespace fs = std::filesystem;

namespace {

bool write_file(std::string const& path, std::span<std::uint8_t const> bytes)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    auto n = fwrite(bytes.data(), 1, bytes.size(), f);
    return fclose(f) == 0 && n == bytes.size();
}

std::optional<std::vector<std::uint8_t>> read_file(std::string const& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    char buf[16384];
    std::size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    fclose(f);
    return out;
}

}  // namespace

AemModelStore::AemModelStore(std::string directory)
    : directory_(std::move(directory))
{
    std::error_code ec;
    fs::create_directories(directory_, ec);
}

std::string AemModelStore::blob_path(std::string_view fingerprint) const
{
    return directory_ + "/" + std::string(fingerprint) + ".aem";
}

std::string AemModelStore::symbols_path(std::string_view fingerprint) const
{
    return directory_ + "/" + std::string(fingerprint) + ".symbols.tsv";
}

std::optional<std::string> AemModelStore::store(std::span<AemRawDescriptor const> descriptors)
{
    auto fingerprint = aem_fingerprint(descriptors);
    if (contains(fingerprint)) {
        return fingerprint;
    }

    AemBlobWriter writer;
    for (auto const& d : descriptors) {
        writer.add(d.configuration, d.descriptor_type, d.descriptor_index, d.bytes);
    }
    if (!write_file(blob_path(fingerprint), writer.write())) {
        return std::nullopt;
    }

    auto symbols = derive_aem_symbols(descriptors);
    std::string text =
        "# aem-symbols v1\n# generator " + std::to_string(AEM_SYMBOL_GENERATOR_VERSION) + "\n# fingerprint " + fingerprint + "\n";
    for (auto const& s : symbols) {
        char line[64];
        snprintf(line, sizeof line, "%u\t%04x\t%u\t", s.configuration, s.descriptor_type, s.descriptor_index);
        text += line;
        text += s.symbol;
        text += s.orphan ? "\torphan\n" : "\tok\n";
    }
    if (!write_file(symbols_path(fingerprint), {reinterpret_cast<std::uint8_t const*>(text.data()), text.size()})) {
        return std::nullopt;
    }
    return fingerprint;
}

bool AemModelStore::contains(std::string_view fingerprint) const
{
    std::error_code ec;
    return fs::exists(blob_path(fingerprint), ec) && fs::exists(symbols_path(fingerprint), ec);
}

std::optional<std::vector<std::uint8_t>> AemModelStore::load_blob(std::string_view fingerprint) const
{
    return read_file(blob_path(fingerprint));
}

std::optional<std::vector<AemSymbol>> AemModelStore::load_symbols(std::string_view fingerprint) const
{
    auto bytes = read_file(symbols_path(fingerprint));
    if (!bytes) {
        return std::nullopt;
    }
    std::string_view text(reinterpret_cast<char const*>(bytes->data()), bytes->size());
    std::vector<AemSymbol> out;
    std::size_t at = 0;
    while (at < text.size()) {
        auto end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        auto line = text.substr(at, end - at);
        at = end + 1;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        AemSymbol s;
        unsigned config = 0, type = 0, index = 0;
        char symbol[128] = {};
        char flag[16] = {};
        if (sscanf(std::string(line).c_str(), "%u\t%x\t%u\t%127[^\t]\t%15s", &config, &type, &index, symbol, flag) != 5) {
            continue;
        }
        s.configuration = std::uint16_t(config);
        s.descriptor_type = std::uint16_t(type);
        s.descriptor_index = std::uint16_t(index);
        s.symbol = symbol;
        s.orphan = std::string_view(flag) == "orphan";
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<std::string> AemModelStore::list() const
{
    std::vector<std::string> out;
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator(directory_, ec)) {
        auto path = entry.path();
        if (path.extension() == ".aem") {
            out.push_back(path.stem().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace statusbar::nanoavb
