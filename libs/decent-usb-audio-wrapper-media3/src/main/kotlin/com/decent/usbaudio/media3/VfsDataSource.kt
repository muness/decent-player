package com.decent.usbaudio.media3

import android.net.Uri
import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.C
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.BaseDataSource
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSpec
import org.apache.commons.vfs2.FileObject
import org.apache.commons.vfs2.FileSystemOptions
import org.apache.commons.vfs2.VFS
import org.apache.commons.vfs2.provider.sftp.SftpFileSystemConfigBuilder
import java.io.InputStream

/**
 * Media3 [DataSource] backed by Apache Commons VFS.
 *
 * Supports any protocol that Commons VFS handles:
 * - `sftp://user:pass@host/path/to/file.flac`
 * - `ftp://user:pass@host/path/to/file.flac`
 * - `ftps://user:pass@host/path/to/file.flac`
 *
 * The wrapper's [UsbAudioSink] treats these as non-local URIs, so playback
 * uses the ExoPlayer pipeline (FlacExtractor or FFmpeg) → USB bit-perfect.
 *
 * Usage: register [Factory] in your ExoPlayer's [DefaultDataSource.Factory]:
 * ```kotlin
 * val vfsFactory = VfsDataSource.Factory()
 * val dataSourceFactory = DefaultDataSource.Factory(context, vfsFactory)
 * val mediaSourceFactory = DefaultMediaSourceFactory(dataSourceFactory)
 * val player = ExoPlayer.Builder(context)
 *     .setMediaSourceFactory(mediaSourceFactory)
 *     .build()
 * ```
 */
@OptIn(UnstableApi::class)
class VfsDataSource : BaseDataSource(/* isNetwork = */ true) {

    private var fileObject: FileObject? = null
    private var inputStream: InputStream? = null
    private var bytesRemaining: Long = 0
    private var opened: Boolean = false
    private var dataSpec: DataSpec? = null

    override fun open(dataSpec: DataSpec): Long {
        this.dataSpec = dataSpec
        // Decode URI — VFS expects decoded paths, not %20-encoded ones
        val rawUri = dataSpec.uri.toString()
        val decodedUri = java.net.URLDecoder.decode(rawUri, "UTF-8")
        Log.i(TAG, "open: $decodedUri (position=${dataSpec.position}, length=${dataSpec.length})")

        try {
            val opts = FileSystemOptions()
            SftpFileSystemConfigBuilder.getInstance().apply {
                setStrictHostKeyChecking(opts, "no")
                setPreferredAuthentications(opts, "password,keyboard-interactive")
                setConnectTimeout(opts, java.time.Duration.ofSeconds(10))
                setSessionTimeout(opts, java.time.Duration.ofSeconds(30))
            }

            val manager = VFS.getManager()

            // Try the URI as-is first, then with relative path (SFTP may chroot to home)
            var file = manager.resolveFile(decodedUri, opts)
            if (!file.exists()) {
                Log.w(TAG, "Not found with absolute path, trying relative...")
                // Strip /home/user/ prefix if present — SFTP chroot
                val relativeUri = decodedUri.replace(Regex("(sftp://[^/]+)/home/[^/]+/"), "$1/")
                Log.i(TAG, "Trying relative: $relativeUri")
                file = manager.resolveFile(relativeUri, opts)
            }
            fileObject = file

            if (!file.exists()) {
                Log.e(TAG, "File not found with any path variant")
                throw java.io.FileNotFoundException("VFS file not found: $decodedUri")
            }

            val fileSize = file.content.size
            inputStream = file.content.inputStream

            // Seek to requested position
            if (dataSpec.position > 0) {
                val skipped = inputStream!!.skip(dataSpec.position)
                if (skipped < dataSpec.position) {
                    Log.w(TAG, "Skip incomplete: requested=${dataSpec.position} skipped=$skipped")
                }
            }

            bytesRemaining = if (dataSpec.length != C.LENGTH_UNSET.toLong()) {
                dataSpec.length
            } else {
                fileSize - dataSpec.position
            }

            opened = true
            transferStarted(dataSpec)
            Log.i(TAG, "opened: size=$fileSize bytesRemaining=$bytesRemaining")
            return bytesRemaining

        } catch (e: Exception) {
            Log.e(TAG, "open failed: ${e.message}", e)
            throw java.io.IOException("VFS open failed for $uri", e)
        }
    }

    override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
        if (bytesRemaining == 0L) return C.RESULT_END_OF_INPUT

        val stream = inputStream ?: return C.RESULT_END_OF_INPUT
        val toRead = minOf(length.toLong(), bytesRemaining).toInt()

        val bytesRead = stream.read(buffer, offset, toRead)
        if (bytesRead == -1) {
            return C.RESULT_END_OF_INPUT
        }

        bytesRemaining -= bytesRead
        bytesTransferred(bytesRead)
        return bytesRead
    }

    override fun getUri(): Uri? = dataSpec?.uri

    override fun close() {
        try {
            inputStream?.close()
            fileObject?.close()
        } catch (e: Exception) {
            Log.w(TAG, "close error: ${e.message}")
        } finally {
            inputStream = null
            fileObject = null
            if (opened) {
                opened = false
                transferEnded()
            }
        }
    }

    /**
     * Factory for [VfsDataSource]. Only handles sftp://, ftp://, ftps:// URIs.
     * For other schemes, returns null so the default DataSource handles them.
     */
    class Factory : DataSource.Factory {
        override fun createDataSource(): DataSource = VfsDataSource()
    }

    companion object {
        private const val TAG = "VfsDataSource"

        /** Schemes handled by this DataSource. */
        val SUPPORTED_SCHEMES = setOf("sftp", "ftp", "ftps")

        /** Check if a URI should be handled by VfsDataSource. */
        fun supportsUri(uri: Uri): Boolean {
            return uri.scheme?.lowercase() in SUPPORTED_SCHEMES
        }
    }
}
