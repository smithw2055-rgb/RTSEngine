#pragma once

#include <RTSEngine/Ecs/ComponentPool.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rts::ecs {

using ComponentTypeId = std::uint32_t;
using ComponentSchemaVersion = std::uint16_t;

struct ComponentSchemaDescriptor final {
    ComponentTypeId typeId{};
    ComponentSchemaVersion version{};
    std::string name;
};

class ComponentSchemaRegistry final {
    struct IEntry {
        IEntry(ComponentSchemaDescriptor value, std::type_index type)
            : descriptor(std::move(value)), cppType(type) {}
        virtual ~IEntry() = default;

        virtual void writePayload(foundation::BinaryWriter& writer,
                                  const void* value) const = 0;
        virtual bool readPayload(foundation::BinaryReader& reader,
                                 ComponentSchemaVersion storedVersion,
                                 void* value) const = 0;
        virtual bool readIntoPool(foundation::BinaryReader& reader,
                                  ComponentSchemaVersion storedVersion,
                                  Entity entity,
                                  IComponentPool& pool) const = 0;
        virtual void hashValue(foundation::CanonicalHash& hash,
                               const void* value) const = 0;
        virtual std::unique_ptr<IComponentPool> createPool() const = 0;

        ComponentSchemaDescriptor descriptor;
        std::type_index cppType;
    };

    template<class T>
    struct Entry final : IEntry {
        using Save = std::function<void(foundation::BinaryWriter&, const T&)>;
        using Load = std::function<bool(foundation::BinaryReader&,
                                        ComponentSchemaVersion, T&)>;
        using Hash = std::function<void(foundation::CanonicalHash&, const T&)>;

        Entry(ComponentSchemaDescriptor descriptor, Save save, Load load, Hash hash)
            : IEntry(std::move(descriptor), std::type_index(typeid(T))),
              save(std::move(save)), load(std::move(load)), hash(std::move(hash)) {}

        void writePayload(foundation::BinaryWriter& writer,
                          const void* value) const override {
            save(writer, *static_cast<const T*>(value));
        }

        bool readPayload(foundation::BinaryReader& reader,
                         ComponentSchemaVersion storedVersion,
                         void* value) const override {
            return load(reader, storedVersion, *static_cast<T*>(value));
        }

        bool readIntoPool(foundation::BinaryReader& reader,
                          ComponentSchemaVersion storedVersion,
                          Entity entity,
                          IComponentPool& componentPool) const override {
            auto* typedPool = dynamic_cast<ComponentPoolModel<T>*>(&componentPool);
            if (!typedPool) {
                return false;
            }
            T value{};
            if (!load(reader, storedVersion, value)) {
                return false;
            }
            typedPool->pool.emplace(entity, std::move(value));
            return true;
        }

        void hashValue(foundation::CanonicalHash& writer,
                       const void* value) const override {
            hash(writer, *static_cast<const T*>(value));
        }

        std::unique_ptr<IComponentPool> createPool() const override {
            return std::make_unique<ComponentPoolModel<T>>();
        }

        Save save;
        Load load;
        Hash hash;
    };

public:
    static constexpr std::uint32_t kMaximumPayloadBytes = 16u * 1024u * 1024u;

    template<class T, class Save, class Load, class Hash>
    bool registerSchema(ComponentTypeId typeId,
                        ComponentSchemaVersion version,
                        std::string name,
                        Save&& save,
                        Load&& load,
                        Hash&& hash) {
        static_assert(std::is_default_constructible<T>::value,
                      "Persistent components must be default constructible");
        static_assert(std::is_move_constructible<T>::value,
                      "Persistent components must be move constructible");
        static_assert(std::is_move_assignable<T>::value,
                      "Persistent components must be move assignable");

        const auto cppType = std::type_index(typeid(T));
        if (frozen_ || typeId == 0 || version == 0 || name.empty() ||
            entriesById_.find(typeId) != entriesById_.end() ||
            entriesByType_.find(cppType) != entriesByType_.end()) {
            return false;
        }

        using TypedEntry = Entry<T>;
        typename TypedEntry::Save saveFunction(std::forward<Save>(save));
        typename TypedEntry::Load loadFunction(std::forward<Load>(load));
        typename TypedEntry::Hash hashFunction(std::forward<Hash>(hash));
        if (!saveFunction || !loadFunction || !hashFunction) {
            return false;
        }

        auto entry = std::make_unique<TypedEntry>(
            ComponentSchemaDescriptor{typeId, version, std::move(name)},
            std::move(saveFunction), std::move(loadFunction), std::move(hashFunction));
        IEntry* pointer = entry.get();
        entriesByType_.emplace(cppType, pointer);
        entriesById_.emplace(typeId, std::move(entry));
        return true;
    }

    void freeze() noexcept { frozen_ = true; }
    bool frozen() const noexcept { return frozen_; }
    std::size_t size() const noexcept { return entriesById_.size(); }

    const ComponentSchemaDescriptor* find(ComponentTypeId typeId) const noexcept {
        const auto iterator = entriesById_.find(typeId);
        return iterator == entriesById_.end() ? nullptr : &iterator->second->descriptor;
    }

    const ComponentSchemaDescriptor* find(std::type_index cppType) const noexcept {
        const auto iterator = entriesByType_.find(cppType);
        return iterator == entriesByType_.end() ? nullptr : &iterator->second->descriptor;
    }

    template<class T>
    const ComponentSchemaDescriptor* find() const noexcept {
        const auto* entry = findEntry<T>();
        return entry ? &entry->descriptor : nullptr;
    }

    std::vector<ComponentSchemaDescriptor> descriptors() const {
        std::vector<ComponentSchemaDescriptor> result;
        result.reserve(entriesById_.size());
        for (const auto& item : entriesById_) {
            result.push_back(item.second->descriptor);
        }
        std::sort(result.begin(), result.end(),
                  [](const ComponentSchemaDescriptor& left,
                     const ComponentSchemaDescriptor& right) {
                      return left.typeId < right.typeId;
                  });
        return result;
    }

    bool writePayload(std::type_index cppType,
                      foundation::BinaryWriter& writer,
                      const void* value) const {
        const auto iterator = entriesByType_.find(cppType);
        if (iterator == entriesByType_.end() || value == nullptr) {
            return false;
        }
        iterator->second->writePayload(writer, value);
        return writer.bytes().size() <= kMaximumPayloadBytes;
    }

    std::unique_ptr<IComponentPool> createPool(ComponentTypeId typeId) const {
        const auto iterator = entriesById_.find(typeId);
        return iterator == entriesById_.end()
            ? nullptr
            : iterator->second->createPool();
    }

    bool readIntoPool(ComponentTypeId typeId,
                      ComponentSchemaVersion storedVersion,
                      foundation::BinaryReader& reader,
                      Entity entity,
                      IComponentPool& pool) const {
        const auto iterator = entriesById_.find(typeId);
        if (iterator == entriesById_.end() || storedVersion == 0 ||
            iterator->second->cppType != pool.cppType()) {
            return false;
        }
        return iterator->second->readIntoPool(
                   reader, storedVersion, entity, pool) && reader.atEnd();
    }

    template<class T>
    bool write(foundation::BinaryWriter& writer, const T& value) const {
        const auto* entry = findEntry<T>();
        if (!entry) return false;

        foundation::BinaryWriter payload;
        entry->writePayload(payload, &value);
        if (payload.bytes().size() > kMaximumPayloadBytes) return false;

        writer.writeU32(entry->descriptor.typeId);
        writer.writeU16(entry->descriptor.version);
        writer.writeU32(static_cast<std::uint32_t>(payload.bytes().size()));
        writer.writeBytes(payload.bytes());
        return true;
    }

    template<class T>
    bool read(foundation::BinaryReader& reader, T& value) const {
        ComponentTypeId typeId = 0;
        ComponentSchemaVersion storedVersion = 0;
        std::uint32_t payloadSize = 0;
        if (!reader.readU32(typeId) || !reader.readU16(storedVersion) ||
            !reader.readU32(payloadSize)) {
            return false;
        }

        std::vector<std::uint8_t> payload;
        if (!reader.readBytes(payloadSize, payload, kMaximumPayloadBytes)) {
            return false;
        }

        const auto iterator = entriesById_.find(typeId);
        if (iterator == entriesById_.end() ||
            iterator->second->cppType != std::type_index(typeid(T)) ||
            storedVersion == 0) {
            return false;
        }

        foundation::BinaryReader payloadReader(payload);
        return iterator->second->readPayload(payloadReader, storedVersion, &value) &&
               payloadReader.atEnd();
    }

    template<class T>
    bool hash(foundation::CanonicalHash& writer, const T& value) const {
        const auto* entry = findEntry<T>();
        if (!entry) return false;
        writer.WriteU32(entry->descriptor.typeId);
        writer.WriteU16(entry->descriptor.version);
        entry->hashValue(writer, &value);
        return true;
    }

private:
    template<class T>
    const IEntry* findEntry() const noexcept {
        const auto iterator = entriesByType_.find(std::type_index(typeid(T)));
        return iterator == entriesByType_.end() ? nullptr : iterator->second;
    }

    std::unordered_map<ComponentTypeId, std::unique_ptr<IEntry>> entriesById_;
    std::unordered_map<std::type_index, IEntry*> entriesByType_;
    bool frozen_{};
};

} // namespace rts::ecs
