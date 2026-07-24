#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rts::assets {

bool NormalizeVirtualPath(std::string_view input, std::string& output);

class VirtualFileSystem {
public:
    virtual ~VirtualFileSystem() = default;

    virtual bool read(std::string_view path,
                      std::vector<std::uint8_t>& output) const = 0;
    virtual bool exists(std::string_view path) const = 0;
};

class MemoryVfs final : public VirtualFileSystem {
public:
    bool write(std::string_view path, std::vector<std::uint8_t> bytes);
    bool remove(std::string_view path);
    bool read(std::string_view path,
              std::vector<std::uint8_t>& output) const override;
    bool exists(std::string_view path) const override;
    std::size_t fileCount() const noexcept;

private:
    struct Entry final {
        std::string path;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Entry>::iterator lower(const std::string& path);
    std::vector<Entry>::const_iterator lower(const std::string& path) const;

    std::vector<Entry> entries_;
};

class MountedVfs final : public VirtualFileSystem {
public:
    bool mount(std::string_view prefix,
               std::shared_ptr<VirtualFileSystem> fileSystem);
    bool unmount(std::string_view prefix);
    bool read(std::string_view path,
              std::vector<std::uint8_t>& output) const override;
    bool exists(std::string_view path) const override;
    std::size_t mountCount() const noexcept;

private:
    struct Mount final {
        std::string prefix;
        std::shared_ptr<VirtualFileSystem> fileSystem;
    };

    static bool matches(const std::string& prefix,
                        const std::string& path,
                        std::string_view& relative) noexcept;

    std::vector<Mount> mounts_;
};

class DirectoryVfs final : public VirtualFileSystem {
public:
    explicit DirectoryVfs(std::filesystem::path root);

    bool read(std::string_view path,
              std::vector<std::uint8_t>& output) const override;
    bool exists(std::string_view path) const override;
    const std::filesystem::path& root() const noexcept;

private:
    std::filesystem::path resolve(std::string_view path) const;

    std::filesystem::path root_;
};

} // namespace rts::assets
