package com.decent.usbaudio

/**
 * One AudioStreaming alternate setting, as parsed from the device's raw USB
 * descriptors.
 *
 * Everything here is descriptor fact. Nothing is inferred from sample rate,
 * product name, or a byte-rate coincidence: [isRawData] is true only when the
 * class-specific AS_GENERAL descriptor says so. A PCM alt setting never
 * becomes DSD because its byte rate happens to match.
 */
data class UsbAsAltSetting(
    /** bAlternateSetting on the AudioStreaming interface. */
    val altSetting: Int,
    /** bSubslotSize (UAC2) or bSubframeSize (UAC1), in bytes. */
    val subslotSize: Int,
    /** bBitResolution: bits actually used inside the subslot. */
    val bitResolution: Int,
    /**
     * UAC2 `bmFormats` from AS_GENERAL, or the UAC1 `wFormatTag` widened to
     * 32 bits. `0x00000001` is PCM; D31 (`0x80000000`) is RAW_DATA.
     */
    val formatBits: Int
) {

    /**
     * True when this alt setting carries an opaque bitstream rather than PCM:
     * UAC2 `bmFormats` bit D31 (RAW_DATA), or UAC1 `wFormatTag` 0x0008.
     *
     * RAW_DATA is how UAC2 DACs advertise a native-DSD alt setting. The
     * driver does not stream it yet, but it must not be mistaken for a PCM
     * alt setting when picking the best bit depth.
     */
    val isRawData: Boolean
        get() = (formatBits and BM_FORMATS_RAW_DATA) != 0 || formatBits == FORMAT_TAG_DSD

    /** True when AS_GENERAL advertises PCM (UAC2 D0, or UAC1 wFormatTag 0x0001). */
    val isPcm: Boolean
        get() = !isRawData && (formatBits and BM_FORMATS_PCM) != 0

    fun describe(): String =
        "alt=$altSetting subslot=$subslotSize bits=$bitResolution " +
            "bmFormats=0x${formatBits.toUInt().toString(16).padStart(8, '0')} " +
            (if (isRawData) "RAW_DATA(D31)" else if (isPcm) "PCM" else "other")

    companion object {
        /** UAC2 Type I `bmFormats` D0: PCM. */
        const val BM_FORMATS_PCM = 0x00000001

        /**
         * UAC2 Type I `bmFormats` D31: RAW_DATA.
         * `0x80000000` does not fit a signed Int literal, and D31 is exactly
         * the sign bit, so [Int.MIN_VALUE] is the same 32-bit pattern.
         */
        const val BM_FORMATS_RAW_DATA: Int = Int.MIN_VALUE

        /** UAC1 `wFormatTag` used by DSD-capable devices. */
        const val FORMAT_TAG_DSD = 0x0008
    }
}
