#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rts::network {

using NetworkEndpointId = std::uint32_t;
using NetworkChannelId = std::uint16_t;
using NetworkPacketSequence = std::uint64_t;

inline constexpr std::size_t kDefaultNetworkMtu = 1200u;

struct NetworkAddress final {
    std::string host;
    std::uint16_t port{};
};

struct TransportDatagram final {
    NetworkEndpointId source{};
    NetworkEndpointId destination{};
    NetworkChannelId channel{};
    NetworkPacketSequence sequence{};
    std::uint64_t sentAtMs{};
    std::uint64_t receivedAtMs{};
    std::vector<std::uint8_t> payload;
};

enum class TransportSendResult : std::uint8_t {
    Accepted,
    InvalidEndpoint,
    PayloadTooLarge,
    NotOpen,
    SocketError
};

class INetworkTransport {
public:
    virtual ~INetworkTransport() = default;

    virtual NetworkEndpointId localEndpoint() const noexcept = 0;
    virtual std::uint64_t nowMs() const noexcept = 0;
    virtual std::size_t maximumPayloadBytes() const noexcept = 0;
    virtual bool open() const noexcept = 0;

    virtual TransportSendResult send(
        NetworkEndpointId destination,
        NetworkChannelId channel,
        const std::vector<std::uint8_t>& payload) = 0;

    virtual bool poll(TransportDatagram& datagram) = 0;
    virtual void update(std::uint64_t nowMs) = 0;
};

} // namespace rts::network
