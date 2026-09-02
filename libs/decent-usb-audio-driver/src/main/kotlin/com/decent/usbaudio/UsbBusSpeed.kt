package com.decent.usbaudio

/**
 * USB bus speed, and the one question this driver asks of it: does the bus
 * schedule in 125us microframes?
 *
 * The isochronous pipeline assumes high-speed timing throughout. Packet sizes
 * come from `sampleRate / 8000`, the reap loop polls once per 125us, and the
 * asynchronous feedback endpoint is decoded as Q16.16 frames per microframe.
 * On a full-speed bus a frame is 1ms, `bInterval` counts whole frames, and
 * feedback is 10.14 frames per frame — so running the same code there would
 * silently mis-clock the DAC rather than fail loudly.
 *
 * Rather than half-implement full-speed timing, [UsbAudioDevice.openDevice]
 * refuses. The constants match the Linux `enum usb_device_speed` that
 * `USBDEVFS_GET_SPEED` returns.
 */
object UsbBusSpeed {

    const val UNKNOWN = 0
    const val LOW = 1
    const val FULL = 2
    const val HIGH = 3
    const val WIRELESS = 4
    const val SUPER = 5
    const val SUPER_PLUS = 6

    /** Where a resolved speed came from, for logging. */
    const val SOURCE_IOCTL = "USBDEVFS_GET_SPEED"
    const val SOURCE_DESCRIPTORS = "descriptor heuristic"
    const val SOURCE_NONE = "undetermined"

    /** Largest wMaxPacketSize a full-speed isochronous endpoint may declare. */
    const val FULL_SPEED_MAX_ISO_PACKET = 1023

    fun name(speed: Int): String = when (speed) {
        LOW -> "low-speed"
        FULL -> "full-speed"
        HIGH -> "high-speed"
        WIRELESS -> "wireless"
        SUPER -> "super-speed"
        SUPER_PLUS -> "super-speed-plus"
        else -> "unknown"
    }

    /**
     * True when the bus schedules in 125us microframes, which is what the
     * packet timing and feedback decoding assume. SuperSpeed and SuperSpeed+
     * keep the same 125us service interval and the same Q16.16 feedback
     * format, so they are treated as high-speed here.
     */
    fun usesMicroframeTiming(speed: Int): Boolean =
        speed == HIGH || speed == SUPER || speed == SUPER_PLUS

    /**
     * Map a `USBDEVFS_GET_SPEED` result. Negative values mean the ioctl
     * failed, and anything outside the enum is treated as [UNKNOWN].
     */
    fun fromIoctl(result: Int): Int =
        if (result < LOW || result > SUPER_PLUS) UNKNOWN else result

    /**
     * Fallback used only when the ioctl fails. A full-speed isochronous
     * endpoint cannot declare more than 1023 bytes per packet, so a larger
     * value on a USB 2.0 device proves high-speed. Anything else stays
     * [UNKNOWN]: this can confirm high-speed but can never rule it out, and a
     * device whose speed is merely plausible is not good enough to clock a DAC
     * against.
     *
     * @param bcdUsb        `bcdUSB` from the device descriptor
     * @param maxPacketSize wMaxPacketSize of the selected isochronous endpoint
     */
    fun fromDescriptors(bcdUsb: Int, maxPacketSize: Int): Int =
        if (bcdUsb >= 0x0200 && maxPacketSize > FULL_SPEED_MAX_ISO_PACKET) HIGH else UNKNOWN
}
