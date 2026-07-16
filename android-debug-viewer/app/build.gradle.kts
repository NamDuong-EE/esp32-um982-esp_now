plugins {
    id("com.android.application")
}

android {
    namespace = "asia.aitogy.roverdebug"
    compileSdk = 35

    defaultConfig {
        applicationId = "asia.aitogy.roverdebug"
        minSdk = 23
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation("androidx.core:core:1.15.0")
    implementation("com.github.mik3y:usb-serial-for-android:3.10.0")
}
