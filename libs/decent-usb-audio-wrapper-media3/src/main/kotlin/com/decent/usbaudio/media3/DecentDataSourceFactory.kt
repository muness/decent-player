package com.decent.usbaudio.media3

import android.content.Context
import androidx.annotation.OptIn
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DefaultDataSource

/**
 * Composite [DataSource.Factory] that routes between:
 * - [SftpDataSource] for `sftp://`, `ftp://`, `ftps://` URIs
 * - [DefaultDataSource] for everything else (`file://`, `content://`, `http://`, `https://`)
 *
 * Usage with ExoPlayer:
 * ```kotlin
 * val dataSourceFactory = DecentDataSourceFactory(context)
 * val mediaSourceFactory = DefaultMediaSourceFactory(dataSourceFactory)
 * val player = ExoPlayer.Builder(context)
 *     .setMediaSourceFactory(mediaSourceFactory)
 *     .build()
 * ```
 */
@OptIn(UnstableApi::class)
class DecentDataSourceFactory(context: Context) : DataSource.Factory {

    private val defaultFactory = DefaultDataSource.Factory(context)
    private val vfsFactory = SftpDataSource.Factory()

    override fun createDataSource(): DataSource {
        // Returns a routing DataSource that delegates based on URI scheme
        return RoutingDataSource(defaultFactory.createDataSource(), vfsFactory.createDataSource())
    }

    /**
     * DataSource that inspects the URI on open() and delegates to the appropriate backend.
     */
    private class RoutingDataSource(
        private val defaultSource: DataSource,
        private val vfsSource: DataSource
    ) : DataSource by defaultSource {

        private var activeSource: DataSource? = null

        @OptIn(UnstableApi::class)
        override fun open(dataSpec: androidx.media3.datasource.DataSpec): Long {
            val source = if (SftpDataSource.supportsUri(dataSpec.uri)) vfsSource else defaultSource
            activeSource = source
            return source.open(dataSpec)
        }

        override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
            return activeSource?.read(buffer, offset, length)
                ?: defaultSource.read(buffer, offset, length)
        }

        override fun getUri(): android.net.Uri? = activeSource?.uri

        override fun close() {
            activeSource?.close()
            activeSource = null
        }
    }
}
