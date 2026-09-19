# Keep Xposed hooks
-keep class com.itsme.itsanon.MainHook
-keep class * implements de.robv.android.xposed.IXposedHookLoadPackage

# Keep license/native JNI classes so native methods are not stripped/obfuscated
-keepclasseswithmembernames class com.itsme.itsanon.security.LicenseGuard {
    native <methods>;
}
-keep class com.itsme.itsanon.security.LicenseGuard { *; }
-keep class com.itsme.itsanon.manager.ModuleManager { *; }

# Keep all hook classes
-keep class com.itsme.itsanon.hooks.** { *; }
-keep class com.itsme.itsanon.gstreamer.** { *; }
-keep class com.itsme.itsanon.libyuv.** { *; }
-keep class com.itsme.itsanon.router.** { *; }

# Keep Camera classes (for hooks)
-keep class android.hardware.camera2.** { *; }
-keep class android.hardware.Camera { *; }
-keep class androidx.camera.core.** { *; }

# Keep GStreamer JNI callback interface (native code calls these by name)
-keep interface com.itsme.itsanon.gstreamer.GStreamerDecoder$FrameCallback { *; }
-keepclasseswithmembernames class com.itsme.itsanon.gstreamer.GStreamerDecoder {
    native <methods>;
}
-keepclasseswithmembernames class com.itsme.itsanon.libyuv.LibYuv {
    native <methods>;
}

# Keep ViewModels
-keep class com.itsme.itsanon.ui.** { *; }

# Keep Timber
-dontwarn com.jakewharton.timber.**
-keep class com.jakewharton.timber.** { *; }

# Keep Xposed classes
-keepnames class * implements de.robv.android.xposed.IXposedHookLoadPackage
-keep class de.robv.android.xposed.** { *; }

# Keep companion objects
-keepclassmembers class * {
    public static final ** Companion;
}

# Keep ViewBinding classes
-keep class com.itsme.itsanon.databinding.** { *; }

# Keep R class
-keep class com.itsme.itsanon.R$* { *; }

# Keep Application class
-keep class com.itsme.itsanon.FaceGateApplication { *; }

# Don't obfuscate Xposed entry point
-keep public class * extends de.robv.android.xposed.IXposedHookLoadPackage

# Remove debug logs in release
-assumenosideeffects class android.util.Log {
    public static *** d(...);
    public static *** v(...);
}
  