#pragma once

#include <RTSEngine/Ecs/ComponentSchema.h>
#include <RTSEngine/Rts/VisionTypes.h>

#include <cstdint>

namespace rts::gameplay {

inline bool RegisterVisionComponentSchema(
    ecs::ComponentSchemaRegistry& schemas) {
    return schemas.registerSchema<VisionSource>(
        0x52545312u,
        1u,
        "rts.VisionSource",
        [](foundation::BinaryWriter& writer, const VisionSource& value) {
            writer.writeI32(value.range);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           VisionSource& value) {
            return version == 1u && reader.readI32(value.range) &&
                   value.range >= 0 && value.range <= 32768;
        },
        [](foundation::CanonicalHash& hash, const VisionSource& value) {
            hash.WriteI32(value.range);
        });
}

} // namespace rts::gameplay
