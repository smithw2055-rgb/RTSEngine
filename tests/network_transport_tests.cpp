#include <RTSEngine/Network/Fragmentation.h>
#include <RTSEngine/Network/LoopbackTransport.h>
#include <RTSEngine/Network/ReliableChannel.h>
#include <RTSEngine/Network/UdpTransport.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using namespace rts::network;

void require(bool condition) {
    if (!condition) std::abort();
}

void testLoopbackLatency() {
    LoopbackNetworkConfig config;
    config.baseLatencyMs = 5;
    LoopbackNetworkHub hub(config);
    LoopbackTransport first(hub, 1);
    LoopbackTransport second(hub, 2);
    require(first.open() && second.open());
    require(first.send(2, 7, {1, 2, 3}) == TransportSendResult::Accepted);
    TransportDatagram datagram;
    second.update(4);
    require(!second.poll(datagram));
    second.update(5);
    require(second.poll(datagram));
    require(datagram.source == 1 && datagram.destination == 2);
    require(datagram.channel == 7);
    require(datagram.payload == std::vector<std::uint8_t>({1, 2, 3}));
}

void testReliableDeliveryUnderLossAndReorder() {
    LoopbackNetworkConfig networkConfig;
    networkConfig.baseLatencyMs = 2;
    networkConfig.jitterMs = 5;
    networkConfig.reorderDelayMs = 8;
    networkConfig.lossBasisPoints = 2000;
    networkConfig.duplicateBasisPoints = 1500;
    networkConfig.randomSeed = 1234567;
    LoopbackNetworkHub hub(networkConfig);
    LoopbackTransport first(hub, 1);
    LoopbackTransport second(hub, 2);

    ReliableChannelConfig reliableConfig;
    reliableConfig.resendIntervalMs = 5;
    reliableConfig.maximumAttempts = 200;
    reliableConfig.maximumInFlight = 32;
    reliableConfig.maximumMessageBytes = 128;
    ReliableChannel toSecond(2, 1, reliableConfig);
    ReliableChannel toFirst(1, 1, reliableConfig);

    for (std::uint8_t value = 0; value < 20; ++value) {
        require(toSecond.queue({value, static_cast<std::uint8_t>(value + 1)}));
    }

    std::vector<ReliableMessage> deliveredAtFirst;
    std::vector<ReliableMessage> deliveredAtSecond;
    for (std::uint64_t now = 0; now < 20000; ++now) {
        first.update(now);
        second.update(now);
        toSecond.flush(first);
        toFirst.flush(second);

        TransportDatagram datagram;
        while (first.poll(datagram)) {
            require(toSecond.receive(datagram, deliveredAtFirst));
        }
        while (second.poll(datagram)) {
            require(toFirst.receive(datagram, deliveredAtSecond));
        }
        toSecond.flush(first);
        toFirst.flush(second);
        if (deliveredAtSecond.size() == 20 && toSecond.pendingCount() == 0) {
            break;
        }
    }

    require(deliveredAtFirst.empty());
    require(deliveredAtSecond.size() == 20);
    for (std::size_t index = 0; index < deliveredAtSecond.size(); ++index) {
        require(deliveredAtSecond[index].sequence == index + 1);
        require(deliveredAtSecond[index].payload.front() == index);
    }
    require(toSecond.pendingCount() == 0);
    require(toSecond.stats().packetsResent > 0);
    require(toFirst.stats().duplicatesSuppressed > 0 ||
            toSecond.stats().packetsResent > 0);
}

void testFragmentationAndReassembly() {
    std::vector<std::uint8_t> source(8192);
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::uint8_t>(index % 251u);
    }
    MessageFragmenter fragmenter(333);
    auto fragments = fragmenter.split(77, source);
    require(fragments.size() > 20);

    MessageReassembler reassembler({512, 10000, 4, 1000});
    std::vector<std::uint8_t> completed;
    for (auto iterator = fragments.rbegin(); iterator != fragments.rend();
         ++iterator) {
        require(reassembler.receive(2, *iterator, 10, completed));
    }
    require(completed == source);
    require(reassembler.pendingAssemblies() == 0);

    completed.clear();
    require(reassembler.receive(2, fragments.front(), 20, completed));
    require(reassembler.receive(2, fragments.front(), 21, completed));
    require(completed.empty());
    reassembler.expire(2000);
    require(reassembler.pendingAssemblies() == 0);
}

void testUdpLoopback() {
    UdpTransportConfig firstConfig;
    firstConfig.localEndpoint = 1;
    firstConfig.bindAddress = {"127.0.0.1", 0};
    firstConfig.maximumPayloadBytes = 512;
    UdpTransportConfig secondConfig = firstConfig;
    secondConfig.localEndpoint = 2;

    UdpTransport first(firstConfig);
    UdpTransport second(secondConfig);
    require(first.open() && second.open());
    require(first.boundPort() != 0 && second.boundPort() != 0);
    require(first.setRemote(2, {"127.0.0.1", second.boundPort()}));
    require(second.setRemote(1, {"127.0.0.1", first.boundPort()}));
    require(first.send(2, 9, {9, 8, 7}) == TransportSendResult::Accepted);

    TransportDatagram datagram;
    bool received = false;
    for (std::uint64_t attempt = 0; attempt < 500 && !received; ++attempt) {
        first.update(attempt);
        second.update(attempt);
        received = second.poll(datagram);
        if (!received) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    require(received);
    require(datagram.source == 1 && datagram.destination == 2);
    require(datagram.channel == 9);
    require(datagram.payload == std::vector<std::uint8_t>({9, 8, 7}));
}

} // namespace

int main() {
    testLoopbackLatency();
    testReliableDeliveryUnderLossAndReorder();
    testFragmentationAndReassembly();
    testUdpLoopback();
    std::cout << "Network transport tests passed\n";
    return EXIT_SUCCESS;
}
