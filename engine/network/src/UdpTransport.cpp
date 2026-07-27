#include <RTSEngine/Network/UdpTransport.h>

#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rts::network {
namespace {

constexpr std::uint32_t kUdpEnvelopeMagic = 0x31544E52u;
constexpr std::uint16_t kUdpEnvelopeVersion = 1u;
constexpr std::size_t kUdpEnvelopeOverhead = 40u;

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

void CloseSocket(NativeSocket socket) noexcept {
    if (socket == kInvalidSocket) return;
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

bool WouldBlock() noexcept {
#if defined(_WIN32)
    const auto error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

bool SetNonBlocking(NativeSocket socket) noexcept {
#if defined(_WIN32)
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const auto flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

std::uint64_t MonotonicMilliseconds() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}

#if defined(_WIN32)
class WinsockRuntime final {
public:
    WinsockRuntime() noexcept {
        WSADATA data{};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockRuntime() {
        if (ready_) WSACleanup();
    }
    bool ready() const noexcept { return ready_; }
private:
    bool ready_{};
};

WinsockRuntime& Winsock() {
    static WinsockRuntime runtime;
    return runtime;
}
#endif

struct ResolvedAddress final {
    sockaddr_storage storage{};
    socklen_t length{};
};

bool ResolveAddress(
    const NetworkAddress& address,
    bool passive,
    ResolvedAddress& output) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;

    const auto service = std::to_string(address.port);
    const char* host = address.host.empty() ? nullptr : address.host.c_str();
    addrinfo* result = nullptr;
    if (getaddrinfo(host, service.c_str(), &hints, &result) != 0 || !result) {
        return false;
    }
    const auto cleanup = [&]() { freeaddrinfo(result); };
    for (auto* current = result; current; current = current->ai_next) {
        if (current->ai_addrlen > sizeof(output.storage)) continue;
        std::memcpy(
            &output.storage, current->ai_addr,
            static_cast<std::size_t>(current->ai_addrlen));
        output.length = static_cast<socklen_t>(current->ai_addrlen);
        cleanup();
        return true;
    }
    cleanup();
    return false;
}

} // namespace

struct UdpTransport::Impl final {
    struct Remote final {
        NetworkEndpointId endpoint{};
        ResolvedAddress address;
    };

    NativeSocket socket{kInvalidSocket};
    UdpTransportConfig config;
    std::vector<Remote> remotes;
    NetworkPacketSequence nextSequence{};
    std::uint64_t nowMs{MonotonicMilliseconds()};
    std::uint16_t boundPort{};

    ~Impl() { CloseSocket(socket); }

    auto lowerRemote(NetworkEndpointId endpoint) noexcept {
        return std::lower_bound(
            remotes.begin(), remotes.end(), endpoint,
            [](const Remote& value, NetworkEndpointId id) {
                return value.endpoint < id;
            });
    }

    auto lowerRemote(NetworkEndpointId endpoint) const noexcept {
        return std::lower_bound(
            remotes.begin(), remotes.end(), endpoint,
            [](const Remote& value, NetworkEndpointId id) {
                return value.endpoint < id;
            });
    }
};

UdpTransport::UdpTransport() : impl_(std::make_unique<Impl>()) {}

UdpTransport::UdpTransport(UdpTransportConfig config)
    : UdpTransport() {
    bind(std::move(config));
}

UdpTransport::~UdpTransport() = default;
UdpTransport::UdpTransport(UdpTransport&&) noexcept = default;
UdpTransport& UdpTransport::operator=(UdpTransport&&) noexcept = default;

bool UdpTransport::bind(UdpTransportConfig config) {
    close();
#if defined(_WIN32)
    if (!Winsock().ready()) return false;
#endif
    if (config.localEndpoint == 0) return false;
    config.maximumPayloadBytes = std::max<std::size_t>(
        64u, config.maximumPayloadBytes);

    ResolvedAddress bindAddress;
    if (!ResolveAddress(config.bindAddress, true, bindAddress)) return false;
    auto socket = ::socket(
        bindAddress.storage.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) return false;

    const auto receiveBytes = static_cast<int>(std::min<std::size_t>(
        config.receiveBufferBytes,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const auto sendBytes = static_cast<int>(std::min<std::size_t>(
        config.sendBufferBytes,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    if (receiveBytes > 0) {
        setsockopt(
            socket, SOL_SOCKET, SO_RCVBUF,
            reinterpret_cast<const char*>(&receiveBytes), sizeof(receiveBytes));
    }
    if (sendBytes > 0) {
        setsockopt(
            socket, SOL_SOCKET, SO_SNDBUF,
            reinterpret_cast<const char*>(&sendBytes), sizeof(sendBytes));
    }

    if (::bind(
            socket,
            reinterpret_cast<const sockaddr*>(&bindAddress.storage),
            bindAddress.length) != 0 ||
        !SetNonBlocking(socket)) {
        CloseSocket(socket);
        return false;
    }

    sockaddr_storage actual{};
    socklen_t actualLength = sizeof(actual);
    std::uint16_t port = config.bindAddress.port;
    if (getsockname(
            socket,
            reinterpret_cast<sockaddr*>(&actual),
            &actualLength) == 0) {
        if (actual.ss_family == AF_INET) {
            port = ntohs(reinterpret_cast<sockaddr_in*>(&actual)->sin_port);
        } else if (actual.ss_family == AF_INET6) {
            port = ntohs(reinterpret_cast<sockaddr_in6*>(&actual)->sin6_port);
        }
    }

    impl_->socket = socket;
    impl_->config = std::move(config);
    impl_->remotes.clear();
    impl_->nextSequence = 0;
    impl_->nowMs = MonotonicMilliseconds();
    impl_->boundPort = port;
    return true;
}

void UdpTransport::close() noexcept {
    if (!impl_) return;
    CloseSocket(impl_->socket);
    impl_->socket = kInvalidSocket;
    impl_->remotes.clear();
    impl_->nextSequence = 0;
    impl_->boundPort = 0;
}

bool UdpTransport::setRemote(
    NetworkEndpointId endpoint,
    NetworkAddress address) {
    if (!impl_ || endpoint == 0 || endpoint == localEndpoint()) return false;
    ResolvedAddress resolved;
    if (!ResolveAddress(address, false, resolved)) return false;
    const auto found = impl_->lowerRemote(endpoint);
    if (found != impl_->remotes.end() && found->endpoint == endpoint) {
        found->address = resolved;
    } else {
        impl_->remotes.insert(found, Impl::Remote{endpoint, resolved});
    }
    return true;
}

bool UdpTransport::removeRemote(NetworkEndpointId endpoint) {
    if (!impl_) return false;
    const auto found = impl_->lowerRemote(endpoint);
    if (found == impl_->remotes.end() || found->endpoint != endpoint) {
        return false;
    }
    impl_->remotes.erase(found);
    return true;
}

std::uint16_t UdpTransport::boundPort() const noexcept {
    return impl_ ? impl_->boundPort : 0;
}

NetworkEndpointId UdpTransport::localEndpoint() const noexcept {
    return impl_ ? impl_->config.localEndpoint : 0;
}

std::uint64_t UdpTransport::nowMs() const noexcept {
    return impl_ ? impl_->nowMs : 0;
}

std::size_t UdpTransport::maximumPayloadBytes() const noexcept {
    return impl_ ? impl_->config.maximumPayloadBytes : 0;
}

bool UdpTransport::open() const noexcept {
    return impl_ && impl_->socket != kInvalidSocket;
}

TransportSendResult UdpTransport::send(
    NetworkEndpointId destination,
    NetworkChannelId channel,
    const std::vector<std::uint8_t>& payload) {
    if (!open()) return TransportSendResult::NotOpen;
    if (payload.size() > impl_->config.maximumPayloadBytes) {
        return TransportSendResult::PayloadTooLarge;
    }
    const auto remote = impl_->lowerRemote(destination);
    if (remote == impl_->remotes.end() || remote->endpoint != destination) {
        return TransportSendResult::InvalidEndpoint;
    }

    foundation::BinaryWriter writer;
    writer.writeU32(kUdpEnvelopeMagic);
    writer.writeU16(kUdpEnvelopeVersion);
    writer.writeU16(channel);
    writer.writeU32(localEndpoint());
    writer.writeU32(destination);
    writer.writeU64(++impl_->nextSequence);
    writer.writeU64(impl_->nowMs);
    writer.writeU32(static_cast<std::uint32_t>(payload.size()));
    writer.writeBytes(payload);
    const auto& bytes = writer.bytes();

    const auto sent = sendto(
        impl_->socket,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(&remote->address.storage),
        remote->address.length);
    return sent == static_cast<int>(bytes.size())
        ? TransportSendResult::Accepted
        : TransportSendResult::SocketError;
}

bool UdpTransport::poll(TransportDatagram& datagram) {
    if (!open()) return false;
    std::vector<std::uint8_t> buffer(
        impl_->config.maximumPayloadBytes + kUdpEnvelopeOverhead);
    sockaddr_storage sourceAddress{};
    socklen_t sourceLength = sizeof(sourceAddress);
    const auto received = recvfrom(
        impl_->socket,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        reinterpret_cast<sockaddr*>(&sourceAddress),
        &sourceLength);
    if (received < 0) {
        (void)WouldBlock();
        return false;
    }
    buffer.resize(static_cast<std::size_t>(received));

    foundation::BinaryReader reader(buffer);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint32_t payloadBytes = 0;
    if (!reader.readU32(magic) ||
        !reader.readU16(version) ||
        !reader.readU16(datagram.channel) ||
        !reader.readU32(datagram.source) ||
        !reader.readU32(datagram.destination) ||
        !reader.readU64(datagram.sequence) ||
        !reader.readU64(datagram.sentAtMs) ||
        !reader.readU32(payloadBytes) ||
        magic != kUdpEnvelopeMagic ||
        version != kUdpEnvelopeVersion ||
        datagram.source == 0 ||
        datagram.destination != localEndpoint() ||
        payloadBytes > impl_->config.maximumPayloadBytes ||
        !reader.readBytes(
            payloadBytes,
            datagram.payload,
            impl_->config.maximumPayloadBytes) ||
        !reader.atEnd()) {
        return false;
    }
    datagram.receivedAtMs = impl_->nowMs;
    return true;
}

void UdpTransport::update(std::uint64_t nowMs) {
    if (impl_ && nowMs >= impl_->nowMs) impl_->nowMs = nowMs;
}

} // namespace rts::network
