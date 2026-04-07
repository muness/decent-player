plugins {
    id("com.android.library")
}

android {
    namespace = "com.decent.usbaudio.media3"
    compileSdk = 36

    defaultConfig {
        minSdk = 29
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    kotlin { jvmToolchain(21) }
}

dependencies {
    api(project(":decent-usb-audio-driver"))
    implementation("androidx.core:core-ktx:1.18.0")
    implementation("androidx.media3:media3-exoplayer:1.9.3")
    implementation("androidx.media3:media3-common:1.9.3")
    implementation("androidx.media3:media3-datasource:1.9.3")

    // Apache Commons VFS for SFTP/FTP streaming support
    implementation("org.apache.commons:commons-vfs2:2.10.0")
    // JSch fork (maintained) — required by Commons VFS for SFTP
    implementation("com.github.mwiede:jsch:0.2.23")
}
