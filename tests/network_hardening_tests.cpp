#include <RTSEngine/Network/ConnectionQuality.h>
#include <RTSEngine/Network/Security.h>
#include <RTSEngine/Network/TrafficControl.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace rts;

void require(bool value) {
    if (!value) std::abort();
}

class TestSecurityProvider final : public network::INetworkSecurityProvider {
public:
    std::size_t maximumTagBytes() const noexcept override { return 8u; }

    bool seal(
        const network::NetworkSecurityRequest& request,
        std::vector<std::uint8_t>& ciphertext,
        std::vector<std::uint8_t>& tag) override {
        if (!request.associatedData || !request.input || request.keyId == 0 ||
            request.nonce == 0) {
            return false;
        }
        ciphertext = *request.input;
        crypt(request.keyId, request.nonce, ciphertext);
        tag = makeTag(request, ciphertext);
        return true;
    }

    bool open(
        const network::NetworkSecurityRequest& request,
        const std::vector<std::uint8_t>& tag,
        std::vector<std::uint8_t>& plaintext) override {
        if (!request.associatedData || !request.input ||
            tag != makeTag(request, *request.input)) {
            return false;
        }
        plaintext = *request.input;
        crypt(request.keyId, request.nonce, plaintext);
        return true;
    }

private:
    static void crypt(
        std::uint64_t keyId,
        std::uint64_t nonce,
        std::vector<std::uint8_t>& bytes) {
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const auto shift = static_cast<unsigned>((index % 8u) * 8u);
            const auto keyByte = static_cast<std::uint8_t>(
                (keyId >> shift) ^ (nonce >> ((7u - index % 8u) * 8u)));
            bytes[index] ^= static_cast<std::uint8_t>(keyByte + index * 17u);
        }
    }

    static std::vector<std::uint8_t> makeTag(
        const network::NetworkSecurityRequest& request,
        const std::vector<std::uint8_t>& ciphertext) {
        foundation::CanonicalHash hash;
        hash.WriteU64(request.keyId);
        hash.WriteU64(request.epoch);
        hash.WriteU64(request.nonce);
        for (const auto byte : *request.associatedData) hash.WriteU8(byte);
        for (const auto byte : ciphertext) hash.WriteU8(byte);
        const auto value = hash.Value();
        std::vector<std::uint8_t> tag(8u);
        for (unsigned shift = 0; shift < 64; shift += 8) {
            tag[shift / 8u] = static_cast<std::uint8_t>(value >> shift);
        }
        return tag;
    }
};

void testSecurityAndReplayProtection() {
    TestSecurityProvider provider;
    network::NetworkSecuritySession sender(
        1, 2, {&provider, 77, 3, true, 4096});
    network::NetworkSecuritySession receiver(
        2, 1, {&provider, 77, 3, true, 4096});
    require(sender.active() && receiver.active());

    const std::vector<std::uint8_t> plaintext{1, 2, 3, 4, 5, 6, 7};
    std::vector<std::uint8_t> packet;
    require(sender.protect(plaintext, packet));
    require(packet != plaintext);

    std::vector<std::uint8_t> decoded;
    require(receiver.open(packet, decoded) ==
            network::NetworkSecurityOpenResult::Accepted);
    require(decoded == plaintext);
    require(receiver.open(packet, decoded) ==
            network::NetworkSecurityOpenResult::ReplayRejected);

    std::vector<std::uint8_t> second;
    require(sender.protect(plaintext, second));
    second.back() ^= 0x5Au;
    require(receiver.open(second, decoded) ==
            network::NetworkSecurityOpenResult::AuthenticationFailed);

    require(receiver.open(plaintext, decoded) ==
            network::NetworkSecurityOpenResult::InvalidContext);
    require(receiver.stats().protectedReceived == 1);
    require(receiver.stats().replayRejected == 1);
    require(receiver.stats().authenticationFailed == 1);
    require(receiver.stats().clearRejected == 1);
}

void testAtomicTrafficLimits() {
    network::TrafficLimitConfig limits;
    limits.packets = {2, 2, 100};
    limits.bytes = {10, 10, 100};
    network::TrafficGovernor governor(limits);

    require(governor.allow(1, 4));
    require(governor.allow(1, 6));
    require(!governor.allow(1, 1));
    require(governor.allow(101, 10));
    require(!governor.allow(101, 1));
    require(governor.stats().acceptedPackets == 3);
    require(governor.stats().acceptedBytes == 20);
    require(governor.stats().rejectedPackets == 2);
}

void testConnectionQuality() {
    network::ConnectionQualityConfig config;
    config.pingIntervalMs = 10;
    config.pingTimeoutMs = 30;
    config.poorRttMs = 100;
    config.criticalRttMs = 200;
    config.poorLossPermille = 200;
    config.criticalLossPermille = 500;
    network::ConnectionQualityTracker tracker(config);

    const auto first = tracker.beginPing(1);
    require(first != 0);
    require(tracker.acknowledge(first, 41));
    require(tracker.snapshot().smoothedRttMs == 40);
    require(tracker.snapshot().grade ==
            network::ConnectionQualityGrade::Excellent);
    require(!tracker.preferReliableDelivery());

    const auto second = tracker.beginPing(51);
    require(second != 0);
    tracker.expire(82);
    require(tracker.snapshot().pingsLost == 1);
    require(tracker.snapshot().lossPermille == 500);
    require(tracker.snapshot().grade ==
            network::ConnectionQualityGrade::Critical);
    require(tracker.preferReliableDelivery());
    require(tracker.recommendedInputDelayTicks(20, 1, 12) >= 2);
}

} // namespace

int main() {
    testSecurityAndReplayProtection();
    testAtomicTrafficLimits();
    testConnectionQuality();
    std::cout << "Network hardening tests passed\n";
    return EXIT_SUCCESS;
}
