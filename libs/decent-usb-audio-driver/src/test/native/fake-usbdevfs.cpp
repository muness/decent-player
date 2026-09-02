/**
 * @file fake-usbdevfs.cpp
 * @brief Fake usbdevfs kernel for the host test. See fake-usbdevfs.h.
 */

#include "fake-usbdevfs.h"

#include <cerrno>
#include <cstring>

#ifndef USBDEVFS_GET_SPEED
#define USBDEVFS_GET_SPEED _IO('U', 31)
#endif

namespace fakeusb {

namespace {

/** Feedback endpoints are IN; audio data flows OUT. */
bool isInputEndpoint(const struct usbdevfs_urb *urb) {
    return (urb->endpoint & 0x80) != 0;
}

}  // namespace

FakeUsbdevfs::FakeUsbdevfs(int busSpeed) : busSpeed_(busSpeed) {
    // Large enough that the transport's pointer comparisons are meaningful and
    // never alias a real slot.
    staleUrbStorage_.assign(sizeof(struct usbdevfs_urb)
                            + 8 * sizeof(struct usbdevfs_iso_packet_desc), 0);
}

UsbAudioBackend FakeUsbdevfs::backend() {
    UsbAudioBackend table;
    table.control = &FakeUsbdevfs::controlThunk;
    table.wait = &FakeUsbdevfs::waitThunk;
    table.context = this;
    return table;
}

void FakeUsbdevfs::setCompletionDelayMicroframes(int microframes) {
    completionDelay_ = microframes < 0 ? 0 : microframes;
}

void FakeUsbdevfs::setFeedbackFramesPerMicroframe(double frames) {
    feedbackFrames_ = frames;
}

void FakeUsbdevfs::requestOutOfOrderSwap() {
    swapNextPair_ = true;
}

void FakeUsbdevfs::injectStaleCompletion() {
    staleNext_ = true;
}

void FakeUsbdevfs::setBusSpeed(int busSpeed) {
    busSpeed_ = busSpeed;
}

std::vector<uint8_t> FakeUsbdevfs::audioPayload() const {
    std::vector<uint8_t> all;
    for (const RecordedPacket &packet : audioPackets_) {
        all.insert(all.end(), packet.bytes.begin(), packet.bytes.end());
    }
    return all;
}

int FakeUsbdevfs::controlThunk(void *context, int, unsigned long request, void *argument) {
    return static_cast<FakeUsbdevfs *>(context)->control(request, argument);
}

void FakeUsbdevfs::waitThunk(void *context, unsigned microseconds) {
    static_cast<FakeUsbdevfs *>(context)->wait(microseconds);
}

void FakeUsbdevfs::wait(unsigned microseconds) {
    // A high-speed microframe is 125 us. Never advance by zero, or a poll loop
    // that waits for a completion would spin without the clock moving.
    std::int64_t steps = static_cast<std::int64_t>(microseconds) / 125;
    microframe_ += steps < 1 ? 1 : steps;
}

int FakeUsbdevfs::control(unsigned long request, void *argument) {
    if (request == static_cast<unsigned long>(USBDEVFS_SUBMITURB)) {
        return submit(static_cast<struct usbdevfs_urb *>(argument));
    }
    if (request == static_cast<unsigned long>(USBDEVFS_REAPURBNDELAY)) {
        return reap(argument);
    }
    if (request == static_cast<unsigned long>(USBDEVFS_DISCARDURB)) {
        return discard(static_cast<struct usbdevfs_urb *>(argument));
    }
    if (request == static_cast<unsigned long>(USBDEVFS_GET_SPEED)) {
        return busSpeed_;
    }
    errno = ENOTTY;
    return -1;
}

int FakeUsbdevfs::submit(struct usbdevfs_urb *urb) {
    if (urb == nullptr) {
        errno = EINVAL;
        return -1;
    }

    const bool feedback = isInputEndpoint(urb);
    if (!feedback) {
        // Copy the payload now: after completion the transport may refill this
        // slot's buffer, so reading it later would not prove what was sent.
        const uint8_t *base = static_cast<const uint8_t *>(urb->buffer);
        int offset = 0;
        for (int index = 0; index < urb->number_of_packets; index++) {
            const int length = static_cast<int>(urb->iso_frame_desc[index].length);
            RecordedPacket packet;
            packet.endpoint = urb->endpoint;
            packet.bytes.assign(base + offset, base + offset + length);
            audioPackets_.push_back(std::move(packet));
            offset += length;
        }
        submittedAudioUrbs_++;
    }

    Pending entry;
    entry.urb = urb;
    entry.dueMicroframe = microframe_ + completionDelay_;
    entry.isFeedback = feedback;
    entry.discarded = false;
    pending_.push_back(entry);
    return 0;
}

int FakeUsbdevfs::chooseCompletion() {
    int first = -1;
    int second = -1;
    for (std::size_t index = 0; index < pending_.size(); index++) {
        const Pending &entry = pending_[index];
        const bool due = entry.discarded || entry.dueMicroframe <= microframe_;
        if (!due) continue;
        if (first < 0) {
            first = static_cast<int>(index);
        } else {
            second = static_cast<int>(index);
            break;
        }
    }

    // Model a host controller retiring URB k+1 before URB k. Only audio URBs
    // are swapped; the feedback URB is a separate endpoint.
    if (swapNextPair_ && first >= 0 && second >= 0
            && !pending_[static_cast<std::size_t>(first)].isFeedback
            && !pending_[static_cast<std::size_t>(second)].isFeedback) {
        swapNextPair_ = false;
        outOfOrderDelivered_++;
        return second;
    }
    return first;
}

void FakeUsbdevfs::completePacketDescriptors(Pending &entry) {
    struct usbdevfs_urb *urb = entry.urb;
    if (entry.isFeedback) {
        // High-speed UAC2 explicit feedback: Q16.16 frames per microframe,
        // little endian.
        const uint32_t encoded =
                static_cast<uint32_t>(feedbackFrames_ * 65536.0 + 0.5);
        uint8_t *buffer = static_cast<uint8_t *>(urb->buffer);
        if (buffer != nullptr && urb->buffer_length >= 4) {
            buffer[0] = static_cast<uint8_t>(encoded & 0xff);
            buffer[1] = static_cast<uint8_t>((encoded >> 8) & 0xff);
            buffer[2] = static_cast<uint8_t>((encoded >> 16) & 0xff);
            buffer[3] = static_cast<uint8_t>((encoded >> 24) & 0xff);
        }
        urb->iso_frame_desc[0].actual_length = feedbackFrames_ > 0.0 ? 4 : 0;
        urb->iso_frame_desc[0].status = 0;
        urb->actual_length = urb->iso_frame_desc[0].actual_length;
        return;
    }

    int total = 0;
    for (int index = 0; index < urb->number_of_packets; index++) {
        // A healthy isochronous OUT transfer sends every byte it was given.
        urb->iso_frame_desc[index].actual_length =
                entry.discarded ? 0 : urb->iso_frame_desc[index].length;
        urb->iso_frame_desc[index].status = entry.discarded ? -ECONNRESET : 0;
        total += static_cast<int>(urb->iso_frame_desc[index].actual_length);
    }
    urb->actual_length = total;
    urb->status = entry.discarded ? -ECONNRESET : 0;
}

int FakeUsbdevfs::reap(void *argument) {
    struct usbdevfs_urb **out = static_cast<struct usbdevfs_urb **>(argument);
    if (out == nullptr) {
        errno = EINVAL;
        return -1;
    }

    if (staleNext_) {
        // A completion left over from an earlier stream on the same fd: a
        // pointer the transport's ring knows nothing about.
        staleNext_ = false;
        staleDelivered_++;
        *out = reinterpret_cast<struct usbdevfs_urb *>(staleUrbStorage_.data());
        return 0;
    }

    const int chosen = chooseCompletion();
    if (chosen < 0) {
        errno = EAGAIN;
        return -1;
    }

    Pending entry = pending_[static_cast<std::size_t>(chosen)];
    pending_.erase(pending_.begin() + chosen);
    completePacketDescriptors(entry);
    *out = entry.urb;
    return 0;
}

int FakeUsbdevfs::discard(struct usbdevfs_urb *urb) {
    for (Pending &entry : pending_) {
        if (entry.urb != urb) continue;
        // The kernel does not drop a discarded URB: it completes with
        // -ECONNRESET and must still be reaped.
        entry.discarded = true;
        discardedUrbs_++;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

}  // namespace fakeusb
