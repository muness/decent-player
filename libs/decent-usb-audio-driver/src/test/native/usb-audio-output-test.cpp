/**
 * @file usb-audio-output-test.cpp
 * @brief Deterministic host test for the isochronous URB pipeline.
 *
 * Runs on any Linux host with usbdevfs headers. No Android, no JNI, no DAC:
 * the driver's kernel interface is replaced by fakeusb::FakeUsbdevfs, which
 * records every isochronous packet it is handed.
 *
 * The question every case here answers is the one the hardware will be asked
 * later: are the bytes that reach the endpoint exactly the bytes the caller
 * supplied?
 *
 *   make            # build and run
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "fake-usbdevfs.h"
#include "usb-audio-output.h"

namespace {

// ── tiny harness ────────────────────────────────────────────────────────

int g_checks = 0;
int g_failures = 0;
const char *g_case = "";

void check(bool condition, const char *expression, int line) {
    g_checks++;
    if (!condition) {
        g_failures++;
        fprintf(stdout, "    FAIL %s:%d  %s\n", g_case, line, expression);
    }
}

template <typename A, typename B>
void checkEqual(const A &actual, const B &expected, const char *expression, int line) {
    g_checks++;
    if (!(actual == static_cast<A>(expected))) {
        g_failures++;
        fprintf(stdout, "    FAIL %s:%d  %s (got %lld, want %lld)\n",
                g_case, line, expression,
                static_cast<long long>(actual), static_cast<long long>(expected));
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(actual, expected) checkEqual((actual), (expected), #actual, __LINE__)

void beginCase(const char *name) {
    g_case = name;
    fprintf(stdout, "  %s\n", name);
}

// ── test rig ────────────────────────────────────────────────────────────

const int kFakeFd = 7;
const int kInterfaceId = 1;
const int kEndpointOut = 0x01;
const int kEndpointFeedback = 0x81;
// The Cayin RU7's advertised wMaxPacketSize.
const int kMaxPacketSize = 776;

class Rig {
public:
    Rig(int sampleRate, int channelCount, int bitDepth)
        : sampleRate_(sampleRate) {
        // Default to the nominal clock so packet sizes are exact unless a case
        // deliberately changes the feedback value.
        fake_.setFeedbackFramesPerMicroframe(static_cast<double>(sampleRate) / 8000.0);
        table_ = fake_.backend();
        ctx_ = usbAudioCreate(kFakeFd, kInterfaceId, kEndpointOut, kEndpointFeedback,
                              sampleRate, channelCount, bitDepth, kMaxPacketSize,
                              &table_);
        if (ctx_ != nullptr) usbAudioStart(ctx_);
    }

    ~Rig() {
        if (ctx_ != nullptr) usbAudioDestroy(ctx_);
    }

    Rig(const Rig &) = delete;
    Rig &operator=(const Rig &) = delete;

    /** Push the whole vector through in ExoPlayer-sized chunks. */
    void writeAll(const std::vector<uint8_t> &data, int chunkBytes = 8192) {
        int consumed = 0;
        const int total = static_cast<int>(data.size());
        while (consumed < total && ctx_->running.load()) {
            int chunk = total - consumed < chunkBytes ? total - consumed : chunkBytes;
            submitPcmToUrbs(ctx_, data.data() + consumed, chunk);
            consumed += chunk;
            CHECK(ctx_->urbsInFlight >= 0);
            CHECK(ctx_->urbsInFlight <= USB_AUDIO_NUM_URBS);
        }
    }

    fakeusb::FakeUsbdevfs &fake() { return fake_; }
    UsbAudioContext *ctx() { return ctx_; }
    int sampleRate() const { return sampleRate_; }

private:
    int sampleRate_;
    fakeusb::FakeUsbdevfs fake_;
    UsbAudioBackend table_{};
    UsbAudioContext *ctx_ = nullptr;
};

// ── payload helpers ─────────────────────────────────────────────────────

/** Deterministic, non-repeating byte pattern so any reorder is visible. */
std::vector<uint8_t> pattern(int byteCount, uint32_t seed = 0x1234567u) {
    std::vector<uint8_t> data(static_cast<std::size_t>(byteCount));
    uint32_t state = seed;
    for (std::size_t i = 0; i < data.size(); i++) {
        state = state * 1664525u + 1013904223u;
        data[i] = static_cast<uint8_t>((state >> 16) & 0xff);
    }
    return data;
}

/** True when `recorded` is exactly the first recorded.size() bytes of `source`. */
bool isPrefixOf(const std::vector<uint8_t> &recorded, const std::vector<uint8_t> &source) {
    if (recorded.size() > source.size()) return false;
    return std::memcmp(recorded.data(), source.data(), recorded.size()) == 0;
}

double averagePacketBytes(const std::vector<fakeusb::RecordedPacket> &packets, std::size_t from) {
    if (from >= packets.size()) return 0.0;
    double total = 0.0;
    for (std::size_t i = from; i < packets.size(); i++) {
        total += static_cast<double>(packets[i].bytes.size());
    }
    return total / static_cast<double>(packets.size() - from);
}

// ── cases ───────────────────────────────────────────────────────────────

// (a) Integer PCM arrives byte-for-byte.
void testPcmByteEquality() {
    beginCase("a. 32-bit stereo 48 kHz: recorded payload == input bytes");

    Rig rig(48000, 2, 32);
    CHECK(rig.ctx() != nullptr);

    // 48000 Hz is exactly 6 frames per microframe, 8 bytes per frame, 8
    // packets per URB: one URB is 384 bytes and nothing is left over.
    const std::vector<uint8_t> input = pattern(384000);
    rig.writeAll(input);

    const std::vector<uint8_t> recorded = rig.fake().audioPayload();
    CHECK_EQ(recorded.size(), input.size());
    CHECK(recorded == input);
    CHECK_EQ(rig.ctx()->residualBytes, 0);
    CHECK_EQ(rig.fake().submittedAudioUrbs(), 1000u);

    for (const fakeusb::RecordedPacket &packet : rig.fake().audioPackets()) {
        CHECK_EQ(packet.bytes.size(), 48u);
        CHECK_EQ(packet.endpoint, kEndpointOut);
        CHECK(static_cast<int>(packet.bytes.size()) <= kMaxPacketSize);
    }

    CHECK_EQ(rig.ctx()->outOfOrderReaps, 0);
    CHECK_EQ(rig.ctx()->staleCompletions, 0);
}

// (b) A fractional frames-per-microframe rate still delivers every byte.
void testFractionalRate() {
    beginCase("b. 44.1 kHz: fractional accumulator emits 5- and 6-frame packets");

    Rig rig(44100, 2, 32);
    CHECK(rig.ctx() != nullptr);

    // 5.5125 frames per microframe at 8 bytes per frame.
    const std::vector<uint8_t> input = pattern(44100 * 8, 0x0badc0deu);
    rig.writeAll(input);

    const std::vector<uint8_t> recorded = rig.fake().audioPayload();
    CHECK(isPrefixOf(recorded, input));
    // Everything the caller handed over is either on the wire or waiting in
    // the residual buffer. Nothing is dropped or duplicated.
    CHECK_EQ(recorded.size() + static_cast<std::size_t>(rig.ctx()->residualBytes),
             input.size());

    bool saw5 = false;
    bool saw6 = false;
    for (const fakeusb::RecordedPacket &packet : rig.fake().audioPackets()) {
        if (packet.bytes.size() == 5u * 8u) saw5 = true;
        if (packet.bytes.size() == 6u * 8u) saw6 = true;
    }
    CHECK(saw5);
    CHECK(saw6);

    // Average packet size tracks the nominal clock.
    const double average = averagePacketBytes(rig.fake().audioPackets(), 800);
    CHECK(std::fabs(average - 5.5125 * 8.0) < 0.2);
}

// (c) The DAC's reported clock changes the packet schedule, not the bytes.
void testFeedbackChangesPacketSizes() {
    beginCase("c. feedback +/-0.5% moves the average packet size, payload unchanged");

    const int rate = 48000;
    const double nominal = rate / 8000.0;  // 6.0 frames per microframe
    const std::vector<uint8_t> input = pattern(384000);

    double averages[3] = {0.0, 0.0, 0.0};
    const double factors[3] = {1.0, 1.005, 0.995};

    for (int i = 0; i < 3; i++) {
        Rig rig(rate, 2, 32);
        CHECK(rig.ctx() != nullptr);
        rig.fake().setFeedbackFramesPerMicroframe(nominal * factors[i]);

        rig.writeAll(input);

        const std::vector<uint8_t> recorded = rig.fake().audioPayload();
        CHECK(isPrefixOf(recorded, input));
        CHECK_EQ(recorded.size() + static_cast<std::size_t>(rig.ctx()->residualBytes),
                 input.size());

        // Skip the packets submitted before the ring filled and the first
        // feedback report was decoded.
        averages[i] = averagePacketBytes(rig.fake().audioPackets(), 800);
    }

    // Nominal: 6 frames per packet at 8 bytes.
    CHECK(std::fabs(averages[0] - 48.0) < 0.01);
    // +0.5%: 6.03 frames per packet.
    CHECK(averages[1] > averages[0]);
    CHECK(std::fabs(averages[1] - 48.24) < 0.2);
    // -0.5%: 5.97 frames per packet.
    CHECK(averages[2] < averages[0]);
    CHECK(std::fabs(averages[2] - 47.76) < 0.2);
}

// (d) A host controller that retires URB k+1 before URB k must not desync the ring.
void testOutOfOrderReap() {
    beginCase("d. out-of-order completion resynchronises instead of drifting");

    Rig rig(48000, 2, 32);
    CHECK(rig.ctx() != nullptr);

    rig.fake().requestOutOfOrderSwap();

    const std::vector<uint8_t> input = pattern(384000);
    rig.writeAll(input);

    CHECK_EQ(rig.fake().outOfOrderCompletions(), 1u);
    // Exactly one reap arrived before the URB submitted ahead of it. The count
    // must not keep climbing: the cursor re-aims at the oldest outstanding URB,
    // so the ring re-aligns instead of treating every later completion as
    // unexpected. Before the fix this counted once per remaining URB.
    CHECK_EQ(rig.ctx()->outOfOrderReaps, 1);
    // One early completion leaves the ring non-contiguous for the rest of the
    // stream, so the submit cursor legitimately steps over a busy slot from
    // time to time. That costs a loop iteration, never a byte.
    CHECK(rig.ctx()->skippedBusySlots >= 1);
    CHECK(rig.ctx()->urbsInFlight >= 0);
    CHECK_EQ(rig.ctx()->staleCompletions, 0);

    // The payload is still in submission order and complete.
    const std::vector<uint8_t> recorded = rig.fake().audioPayload();
    CHECK_EQ(recorded.size(), input.size());
    CHECK(recorded == input);
}

// (e) A completion belonging to no slot must be ignored, not counted as audio.
void testStaleCompletion() {
    beginCase("e. stale completion is ignored and the stream continues");

    Rig rig(48000, 2, 32);
    CHECK(rig.ctx() != nullptr);

    rig.fake().injectStaleCompletion();

    const std::vector<uint8_t> input = pattern(384000);
    rig.writeAll(input);

    CHECK_EQ(rig.fake().staleCompletionsDelivered(), 1u);
    CHECK(rig.ctx()->staleCompletions >= 1);
    // Before the fix this decremented urbsInFlight for a URB that was never
    // ours, eventually driving it negative.
    CHECK(rig.ctx()->urbsInFlight >= 0);
    CHECK(rig.ctx()->running.load());

    const std::vector<uint8_t> recorded = rig.fake().audioPayload();
    CHECK_EQ(recorded.size(), input.size());
    CHECK(recorded == input);
}

// (f) Nothing may still be queued against the fd once the stream drains.
void testDrainLeavesNothingOutstanding() {
    beginCase("f. drain reaps or discards every URB and clears the ring");

    Rig rig(48000, 2, 32);
    CHECK(rig.ctx() != nullptr);

    const std::vector<uint8_t> input = pattern(384000);
    rig.writeAll(input);
    CHECK(rig.ctx()->urbsInFlight > 0);

    usbAudioDrain(rig.ctx());

    CHECK_EQ(rig.ctx()->urbsInFlight, 0);
    CHECK_EQ(rig.ctx()->residualBytes, 0);
    CHECK(!rig.ctx()->feedbackInFlight);
    // Before the fix, audio completions surfacing during the feedback wait
    // were dropped on the floor and their slots stayed marked in flight.
    for (int slot = 0; slot < USB_AUDIO_NUM_URBS; slot++) {
        CHECK(!rig.ctx()->ring[slot].inFlight);
    }
    // The kernel side agrees: nothing is still queued against the fd.
    CHECK_EQ(rig.fake().outstandingUrbs(), 0u);

    // A drained stream can be restarted and still delivers every byte.
    CHECK(usbAudioStart(rig.ctx()));
    const std::vector<uint8_t> second = pattern(38400, 0xfeedu);
    rig.writeAll(second);
    CHECK(rig.ctx()->urbsInFlight >= 0);
}

// (g) The bus-speed query reports what the kernel says, unmodified.
void testBusSpeedQuery() {
    beginCase("g. usbAudioGetBusSpeed reports the kernel's USB_SPEED_* value");

    fakeusb::FakeUsbdevfs fullSpeed(2);
    UsbAudioBackend fullTable = fullSpeed.backend();
    CHECK_EQ(usbAudioGetBusSpeed(kFakeFd, &fullTable), 2);

    fakeusb::FakeUsbdevfs highSpeed(3);
    UsbAudioBackend highTable = highSpeed.backend();
    CHECK_EQ(usbAudioGetBusSpeed(kFakeFd, &highTable), 3);

    fakeusb::FakeUsbdevfs superSpeed(5);
    UsbAudioBackend superTable = superSpeed.backend();
    CHECK_EQ(usbAudioGetBusSpeed(kFakeFd, &superTable), 5);

    // A negative fd never reaches the kernel.
    CHECK(usbAudioGetBusSpeed(-1, &highTable) < 0);
}

}  // namespace

int main() {
    fprintf(stdout, "decent USB audio output, fake usbdevfs backend\n");

    testPcmByteEquality();
    testFractionalRate();
    testFeedbackChangesPacketSizes();
    testOutOfOrderReap();
    testStaleCompletion();
    testDrainLeavesNothingOutstanding();
    testBusSpeedQuery();

    fprintf(stdout, "%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        fprintf(stdout, "usb-audio-output-test FAILED\n");
        return 1;
    }
    fprintf(stdout, "usb-audio-output-test passed\n");
    return 0;
}
