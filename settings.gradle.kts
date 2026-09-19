pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        // Xposed API repository
        maven {
            url = uri("https://api.xposed.info/")
        }
        // JitPack for any other dependencies
        maven {
            url = uri("https://jitpack.io")
        }
        // GStreamer is built from source by the CI workflow; no Maven runtime is used.
        // (Removed: LibVLC Videolan repo, no longer used)
    }
}

rootProject.name = "FaceGate"
include(":app")
