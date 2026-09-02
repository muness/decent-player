/**
 * @file fake-usbdevfs.h
 * @brief A fake usbdevfs kernel for the host test. No Android, no JNI, no DAC.
 *
 * The fake stands in for the kernel behind UsbAudioBackend: it accepts
 * submitted URBs, records every isochronous packet's bytes in submission
 * order, completes URBs on a virtual microframe clock, and can be told to
 * complete out of order, to hand back a pointer that was never submitted, or
 * to report a particular bus speed.
 *
 * Recording happens at submit time, which is the only moment the payload is
 * guaranteed to belong to the pipeline: once a URB completes, the driver is
 * free to refill that slot's buffer.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#include "usb-audio-output.h"

namespace fakeusb {

/** One isochronous packet exactly as it was handed to the fake kernel. */
struct RecordedPacket {
    /** Endpoint address the packet was submitted to. */
    int endpoint = 0;
    /** Payload bytes, copied at submit time. */
    std::vector<uint8_t> bytes;
};

class FakeUsbdevfs {
public:
    /** @param busSpeed value USBDEVFS_GET_SPEED reports (Linux USB_SPEED_*). */
    explicit FakeUsbdevfs(int busSpeed = 3);

    /** Table to hand to usbAudioCreate() / usbAudioGetBusSpeed(). */
    UsbAudioBackend backend();

    // ── configuration ───────────────────────────────────────────────────

    /** Microframes between a submit and the URB becoming reapable. */
    void setCompletionDelayMicroframes(int microframes);

    /**
     * Value the asynchronous feedback endpoint reports, in frames per
     * microframe. Encoded as Q16.16 when the driver reads it, exactly as a
     * real high-speed UAC2 device does.
     */
    void setFeedbackFramesPerMicroframe(double frames);

    /** Complete the second-oldest due URB before the oldest, once. */
    void requestOutOfOrderSwap();

    /** Hand back a pointer that was never submitted, once. */
    void injectStaleCompletion();

    void setBusSpeed(int busSpeed);

    // ── observation ─────────────────────────────────────────────────────

    /** Every audio packet recorded, in submission order. */
    const std::vector<RecordedPacket> &audioPackets() const { return audioPackets_; }

    /** Concatenated audio payload: what the DAC would have received. */
    std::vector<uint8_t> audioPayload() const;

    std::size_t submittedAudioUrbs() const { return submittedAudioUrbs_; }
    std::size_t discardedUrbs() const { return discardedUrbs_; }
    std::size_t staleCompletionsDelivered() const { return staleDelivered_; }
    std::size_t outOfOrderCompletions() const { return outOfOrderDelivered_; }

    /** URBs the fake still holds, i.e. submitted and not yet reaped. */
    std::size_t outstandingUrbs() const { return pending_.size(); }

    /** Virtual microframes elapsed. Advanced only by the driver's waits. */
    std::int64_t microframe() const { return microframe_; }

private:
    struct Pending {
        struct usbdevfs_urb *urb;
        std::int64_t dueMicroframe;
        bool isFeedback;
        bool discarded;
    };

    static int controlThunk(void *context, int fd, unsigned long request, void *argument);
    static void waitThunk(void *context, unsigned microseconds);

    int control(unsigned long request, void *argument);
    void wait(unsigned microseconds);

    int submit(struct usbdevfs_urb *urb);
    int reap(void *argument);
    int discard(struct usbdevfs_urb *urb);

    /** Index into pending_ of the URB to hand back next, or -1 when none is due. */
    int chooseCompletion();
    void completePacketDescriptors(Pending &entry);

    std::deque<Pending> pending_;
    std::vector<RecordedPacket> audioPackets_;

    std::int64_t microframe_ = 0;
    int completionDelay_ = 1;
    int busSpeed_ = 3;
    double feedbackFrames_ = 0.0;

    bool swapNextPair_ = false;
    bool staleNext_ = false;

    std::size_t submittedAudioUrbs_ = 0;
    std::size_t discardedUrbs_ = 0;
    std::size_t staleDelivered_ = 0;
    std::size_t outOfOrderDelivered_ = 0;

    /** Storage for the bogus pointer handed out by injectStaleCompletion(). */
    std::vector<uint8_t> staleUrbStorage_;
};

}  // namespace fakeusb
