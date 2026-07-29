#include <RTSEngine/Scripting/ScriptModPackage.h>

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool readFile(const std::string& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    return input.good() || input.eof();
}

bool loadPackage(
    const std::string& path,
    rts::scripting::ScriptModPackage& package) {
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes)) {
        std::cerr << "rtsmod: cannot read " << path << '\n';
        return false;
    }
    if (!rts::scripting::ScriptModPackageCodec::decode(bytes, package)) {
        std::cerr << "rtsmod: invalid package " << path << '\n';
        return false;
    }
    return true;
}

void printHash(std::uint64_t value) {
    std::cout << "0x" << std::hex << std::setw(16) << std::setfill('0')
              << value << std::dec << std::setfill(' ');
}

void inspect(const rts::scripting::ScriptModPackage& package) {
    const auto& manifest = package.manifest;
    std::cout << "mod: " << manifest.modId << '\n'
              << "version: " << manifest.version << '\n'
              << "name: " << manifest.displayName << '\n'
              << "priority: " << manifest.priority << '\n'
              << "authoritative: "
              << (manifest.authoritative ? "yes" : "no") << '\n'
              << "strict-determinism: "
              << (manifest.strictDeterminism ? "yes" : "no") << '\n'
              << "aot: " << (manifest.allowAot ? "allowed" : "disabled")
              << '\n'
              << "assets: " << package.assets.size() << '\n'
              << "dependencies: " << manifest.dependencies.size() << '\n'
              << "package-hash: ";
    printHash(package.packageHash);
    std::cout << '\n' << "program-hash: ";
    printHash(manifest.scriptIdentity.programContentHash);
    std::cout << '\n' << "host-api-hash: ";
    printHash(manifest.scriptIdentity.hostApiHash);
    std::cout << '\n';
    for (const auto& dependency : manifest.dependencies) {
        std::cout << "requires: " << dependency.modId;
        if (!dependency.version.empty()) {
            std::cout << '@' << dependency.version;
        }
        if (dependency.optional) std::cout << " (optional)";
        std::cout << '\n';
    }
}

void usage() {
    std::cerr
        << "usage:\n"
        << "  rtsmod inspect <package.rtmod>\n"
        << "  rtsmod validate <package.rtmod> [package.rtmod ...]\n"
        << "  rtsmod resolve <package.rtmod> [package.rtmod ...]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    if (command == "inspect") {
        if (argc != 3) {
            usage();
            return 2;
        }
        rts::scripting::ScriptModPackage package;
        if (!loadPackage(argv[2], package)) return 1;
        inspect(package);
        return 0;
    }
    if (command != "validate" && command != "resolve") {
        usage();
        return 2;
    }

    std::vector<rts::scripting::ScriptModPackage> packages;
    packages.reserve(static_cast<std::size_t>(argc - 2));
    for (int index = 2; index < argc; ++index) {
        rts::scripting::ScriptModPackage package;
        if (!loadPackage(argv[index], package)) return 1;
        packages.push_back(std::move(package));
    }
    const auto result = rts::scripting::ScriptModResolver::resolve(packages);
    if (!result.succeeded()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << rts::scripting::ScriptModFailureName(
                             diagnostic.failure)
                      << ": ";
            if (!diagnostic.modId.empty()) {
                std::cerr << diagnostic.modId << ": ";
            }
            std::cerr << diagnostic.message << '\n';
        }
        return 1;
    }

    std::cout << "mod-set-hash: ";
    printHash(result.modSetHash);
    std::cout << '\n';
    if (command == "resolve") {
        for (const auto index : result.loadOrder) {
            std::cout << packages[index].manifest.modId << '@'
                      << packages[index].manifest.version << '\n';
        }
    }
    return 0;
}
