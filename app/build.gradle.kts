import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

val localProps = Properties().also { props ->
    rootProject.file("local.properties").takeIf { it.exists() }
        ?.inputStream()?.use { props.load(it) }
}

val ffmpegDir: String? = localProps.getProperty("ffmpeg.dir") ?: System.getenv("FFMPEG_ROOT")
val opensslDir: String? = localProps.getProperty("openssl.dir") ?: System.getenv("OPENSSL_ROOT")

android {
    namespace  = "com.itsme.amkush"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.itsme.amkush"
        minSdk        = 26
        targetSdk     = 35
        versionCode   = 2
        versionName   = "2.0"

        // FaceGate build version, derived from the git branch + short sha at build
        // time so it's always accurate (e.g. "V4.8.5.4.5"). Written at the top of
        // the log file so you can instantly tell which build a log came from.
        val fgVersion: String = runCatching {
            val p = ProcessBuilder("git", "rev-parse", "--abbrev-ref", "HEAD")
                .redirectErrorStream(true).start()
            p.inputStream.bufferedReader().readText().trim().takeIf { it.isNotEmpty() }
        }.getOrNull() ?: "unknown"
        buildConfigField("String", "FG_VERSION", "\"$fgVersion\"")

        // Short commit sha embedded at build time for diagnostics and log metadata.
        val fgBuild: String = runCatching {
            val p = ProcessBuilder("git", "rev-parse", "--short=10", "HEAD")
                .redirectErrorStream(true).start()
            p.inputStream.bufferedReader().readText().trim().takeIf { it.isNotEmpty() }
        }.getOrNull() ?: "unknown"
        buildConfigField("String", "FG_BUILD", "\"$fgBuild\"")

        nd {
            // arm64 or armeabi-v7a; add x86_64 back only when injector binaries
            // are built for that ABI. Listing x86_64 caused the APK to install
            // on emulators where injection then silently failed.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++14", "-frtti", "-fexceptions")
                val args = mutableListOf<String>()
                if (!ffmpegDir.isNullOrEmpty()) args += "-DFFMPEG_ROOT=$ffmpegDir"
                if (!opensslDir.isNullOrEmpty()) args += "-DOPENSSL_ROOT=$opensslDir"
                // Bake the git branch + short SHA into libframe_producer.so so the
                // FRESH-BUILD-CHECK banner in logcat proves which build is running.
                val fgBranch = runCatching {
                    ProcessBuilder("git", "rev-parse", "--abbrev-ref", "HEAD")
                        .redirectErrorStream(true).start()
                        .inputStream.bufferedReader().readText().trim()
                }.getOrNull()?.takeIf { it.isNotEmpty() } ?: "unknown"
                args += "-DFRAME_BUILD_ID=$fgBranch-$fgBuild"
                if (args.isNotEmpty()) arguments(*args.toTypedArray())
            }
        }
    }

    externalNativeBuild {
        cmake {
            path    = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1+"
        }
    }

    signingConfigs {
        create("release") {
            // Decoded at CI time from KEYSTORE_BASE64 into $HOME/facegate-release.jks.
            // Path/passwords arrive as -P Gradle project properties (env vars are
            // not reliably inherited by the Gradle daemon; user.home can be "/").
            val ksPath = (findProperty("KEYSTORE_PATH") as String?)
                ?: System.getenv("KEYSTORE_PATH")
                ?: "${System.getProperty("user.home")}/facegate-release.jks"
            storeFile     = File(ksPath)
            storePassword = (findProperty("KEYSTORE_PASSWORD") as String?) ?: System.getenv("KEYSTORE_PASSWORD")
            keyAlias      = (findProperty("KEY_ALIAS") as String?) ?: System.getenv("KEY_ALIAS") ?: "facegate"
            keyPassword   = (findProperty("KEY_PASSWORD") as String?) ?: System.getenv("KEY_PASSWORD") ?: System.getenv("KEYSTORE_PASSWORD")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled   = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // Sign release builds with the production keystore. The CI workflow passes
            // -PKEYSTORE_PATH (a Gradle project property), so use its presence to
            // select the release signing config; otherwise fall back to debug signing
            // so local builds still work.
            val hasKeystore = (findProperty("KEYSTORE_PATH") as String?) != null
                || System.getenv("KEYSTORE_PATH") != null
            signingConfig = if (hasKeystore) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
        debug { isMinifyEnabled = false }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose     = true
        buildConfig = true
        aidl        = true
    }

    lint {
        // Release builds run lintVitalRelease by default, which crashes on this
        // AGP/JDK combo (IncompatibleClassChangeError in NonNullableMutableLiveDataDetector).
        // Disable it so assembleRelease can produce the signed APK.
        checkReleaseBuilds = false
        abortOnError = false
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

dependencies {
    implementation("org.jetbrains.kotlin:kotlin-stdlib:2.3.0")
    implementation("org.jetbrains.kotlin:kotlin-reflect:2.3.0")

    implementation("androidx.core:core-ktx:1.16.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.9.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.1")

    val composeBom = platform("androidx.compose:compose-bom:2025.06.00")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.animation:animation")
    implementation("androidx.compose.runtime:runtime")
    implementation("androidx.activity:activity-compose:1.10.1")

    implementation("com.github.bumptech.glide:glide:4.16.0")
    annotationProcessor("com.github.bumptech.glide:compiler:4.16.0")


    // GStreamer is built from upstream source by the CI workflow and linked into the native pipeline.

    implementation("androidx.camera:camera-core:1.4.2")
    implementation("androidx.camera:camera-camera2:1.4.2")

    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.11.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.11.0")
    implementation("androidx.exifinterface:exifinterface:1.3.7")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("com.jakewharton.timber:timber:5.0.1")

    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
}
