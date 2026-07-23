#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rts::assets {

inline bool NormalizeVirtualPath(std::string_view input,
                                 std::string& output) {
    output.clear();
    if (input.empty() || input.front() == '/' || input.front() == '\\') {
        return false;
    }

    std::string segment;
    std::vector<std::string> segments;
    auto flush = [&]() -> bool {
        if (segment.empty()) return true;
        if (segment == "." || segment == "..") return false;
        if (segment.find(':') != std::string::npos) return false;
        segments.push_back(std::move(segment));
        segment.clear();
        return true;
    };

    for (const char character : input) {
        if (character == '/' || character == '\\') {
            if (!flush()) return false;
        } else if (character == '\0') {
            return false;
        } else {
            segment.push_back(character);
        }
    }
    if (!flush() || segments.empty()) return false;

    for (std::size_t index = 0; index < segments.size(); ++index) {
        if (index != 0) output.push_back('/');
        output += segments[index];
    }
    return !output.empty();
}

class VirtualFileSystem {
public:
    virtual ~VirtualFileSystem() = default;

    virtual bool read(std::string_view path,
                      std::vector<std::uint8_t>& output) const = 0;
    virtual bool exists(std::string_view path) const = 0;
};

class MemoryVfs final : public VirtualFileSystem {
public:
    bool write(std::string_view path, std::vector<std::uint8_t> bytes) {
        std::string normalized;
        if (!NormalizeVirtualPath(path, normalized)) return false;
        const auto iterator = lower(normalized);
        if (iterator != entries_.end() && iterator->path == normalized) {
            iterator->bytes = std::move(bytes);
        } else {
            entries_.insert(iterator, {std::move(normalized), std::move(bytes)});
        }
        return true;
    }

    bool remove(std::string_view path) {
        std::string normalized;
        if (!NormalizeVirtualPath(path, normalized)) return false;
        const auto iterator = lower(normalized);
        if (iterator == entries_.end() || iterator->path != normalized) {
            return false;
        }
        entries_.erase(iterator);
        return true;
    }

    bool read(std::string_view path,
              std::vector<std::uint8_t>& output) const override {
        std::string normalized;
        if (!NormalizeVirtualPath(path, normalized)) return false;
        const auto iterator = lower(normalized);
        if (iterator == entries_.end() || iterator->path != normalized) {
            return false;
        }
        output = iterator->bytes;
        return true;
    }

    bool exists(std::string_view path) const override {
        std::string normalized;
        if (!NormalizeVirtualPath(path, normalized)) return false;
        const auto iterator = lower(normalized);
        return iterator != entries_.end() && iterator->path == normalized;
    }

    std::size_t fileCount() const noexcept { return entries_.size(); }

private:
    struct Entry final {
        std::string path;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Entry>::iterator lower(const std::string& path) {
        return std::lower_bound(
            entries_.begin(), entries_.end(), path,
            [](const Entry& value, const std::string& lookup) {
                return value.path < lookup;
            });
    }

    std::vector<Entry>::const_iterator lower(
        const std::string& path) const {
        return std::lower_bound(
            entries_.begin(), entries_.end(), path,
            [](const Entry& value, const std::string& lookup) {
                return value.path < lookup;
            });
    }

    std::vector<Entry> entries_;
};

class MountedVfs final : public VirtualFileSystem {
public:
    bool mount(std::string_view prefix,
               std::shared_ptr<VirtualFileSystem> fileSystem) {
        if (!fileSystem) return false;
        std::string normalized;
        if (!prefix.empty() && !NormalizeVirtualPath(prefix, normalized)) {
            return false;
        }
        const auto existing = std::find_if(
            mounts_.begin(), mounts_.end(),
            [&](const Mount& value) { return value.prefix == normalized; });
        if (existing != mounts_.end()) return false;
        mounts_.push_back({std::move(normalized), std::move(fileSystem)});
        std::sort(mounts_.begin(), mounts_.end(),
                  [](const Mount& a, const Mount& b) {
                      if (a.prefix.size() != b.prefix.size()) {
                          return a.prefix.size() > b.prefix.size();
                      }
                      return a.prefix < b.prefix;
                  });
        return true;
    }

    bool unmount(std::string_view prefix) {
        std::string normalized;
        if (!prefix.empty() && !NormalizeVirtualPath(prefix, normalized)) {
            return false;
        }
        const auto iterator = std::find_if(
            mounts_.begin(), mounts_.end(),
            [&](const Mount& value) { return value.prefix == normalized; });
        if (iterator == mounts_.end()) return false;
        mounts_.erase(iterator);
        return true;
    }

    bool read(std::string_view path,
              std::vector<std::uint8_t>& output) const override {
        std::string normalized;
        if (!NormalizeVirtualPath(path, normalized)) return false;
        for (const auto& mount : mounts_) {
            std::string_view relative;
            if (!matches(mount.prefix, normalized, relative)) continue;
            if (mount.fileSystem->read(relative, output)) return true;
        }
        return false;
    }

    bool exists(std::string_view path) const override {
        std::string normalized;
        if (!NormalizeVirtualPath(path, normalized)) return false;
        for (const auto& mount : mounts_) {
            std::string_view relative;
            if (matches(mount.prefix, normalized, relative) &&
                mount.fileSystem->exists(relative)) {
                return true;
            }
        }
        return false;
    }

    std::size_t mountCount() const noexcept { return mounts_.size(); }

private:
    struct Mount final {
        std::string prefix;
        std::shared_ptr<VirtualFileSystem> fileSystem;
    };

    static bool matches(const std::string& prefix,
                        const std::string& path,
                        std::string_view& relative) noexcept {
        if (prefix.empty()) {
            relative = path;
            return true;
        }
        if (path.size() <= prefix.size() ||
            path.compare(0, prefix.size(), prefix) != 0 ||
            path[prefix.size()] != '/') {
            return false;
        }
        relative = std::string_view(path).substr(prefix.size() + 1u);
        return !relative.empty();
    }

    std::vector<Mount> mounts_;
};

class DirectoryVfs final : public VirtualFileSystem {
public:
    explicit DirectoryVfs(std::filesystem::path root)
        : root_(std::filesystem::weakly_canonical(std::move(root))) {}

    bool read(std::string_view path,
              std::vector<std::uint8_t>& output) const override {
        const auto resolved = resolve(path);
        if (resolved.empty()) return false;
        std::ifstream stream(resolved, std::ios::binary | std::ios::ate);
        if (!stream) return false;
        const auto end = stream.tellg();
        if (end < 0) return false;
        output.resize(static_cast<std::size_t>(end));
        stream.seekg(0, std::ios::beg);
        if (!output.empty()) {
            stream.read(reinterpret_cast<char*>(output.data()),
                        static_cast<std::streamsize>(output.size()));
        }
        return stream.good() || stream.eof();
    }

    bool exists(std::string_view path) const override {
        const auto resolved = resolve(path);
        return !resolved.empty() && std::filesystem::is_regular_file(resolved);
    }

    const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path resolve(std::string_view path) const {
        std::string normalized;
        if (!NormalizeVirtualPath(path, normalized) || root_.empty()) return {};
        const auto candidate = std::filesystem::weakly_canonical(
            root_ / std::filesystem::path(normalized));
        auto rootIterator = root_.begin();
        auto candidateIterator = candidate.begin();
        for (; rootIterator != root_.end();
             ++rootIterator, ++candidateIterator) {
            if (candidateIterator == candidate.end() ||
                *rootIterator != *candidateIterator) {
                return {};
            }
        }
        return candidate;
    }

    std::filesystem::path root_;
};

} // namespace rts::assets
