#pragma once

#include <RTSEngine/Network/Transport.h>

#include <cstdint>
#include <memory>

namespace rts::network {

struct UdpTransportConfig final {
    NetworkEndpointId localEndpoint{};
    NetworkAddress bindAddress{"0.0.0.0", 0};
    std::size_t maximumPayloadBytes{kDefaultNetworkMtu};
    std::size_t receiveBufferBytes{256u * 1024u};
    std::size_t sendBufferBytes{256u * 1024u};
};

class UdpTransport final : public INetworkTransport {
public:
    UdpTransport();
    explicit UdpTransport(UdpTransportConfig config);
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;
    UdpTransport(UdpTransport&&) noexcept;
    UdpTransport& operator=(UdpTransport&&) noexcept;

    bool bind(UdpTransportConfig config);
    void close() noexcept;
    bool setRemote(NetworkEndpointId endpoint, NetworkAddress address);
    bool removeRemote(NetworkEndpointId endpoint);
    std::uint16_t boundPort() const noexcept;

    NetworkEndpointId localEndpoint() const noexcept override;
    std::uint64_t nowMs() const noexcept override;
    std::size_t maximumPayloadBytes() const noexcept override;
    bool open() const noexcept override;

    TransportSendResult send(
        NetworkEndpointId destination,
        NetworkChannelId channel,
        const std::vector<std::uint8_t>& payload) override;

    bool poll(TransportDatagram& datagram) override;
    void update(std::uint64_t nowMs) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rts::network
