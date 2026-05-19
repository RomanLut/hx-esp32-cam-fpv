import org.jetbrains.kotlin.gradle.dsl.JvmTarget

fun readDefine(file: File, name: String): String {
    val regex = Regex("""#define\s+$name\s+"?([^\s"]+)"?""")
    return file.useLines { lines -> lines.mapNotNull { regex.find(it)?.groupValues?.get(1) }.first() }
}

val fwVersion = readDefine(rootProject.file("../components/common/packets.h"), "FW_VERSION")
val packetVersion = readDefine(rootProject.file("../components/common/fec.h"), "PACKET_VERSION")
val appVersion = "$fwVersion.$packetVersion"

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "com.esp32camfpv.androidgs"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.esp32camfpv.androidgs"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = appVersion

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20")
            }
        }

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
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

    kotlin {
        compilerOptions {
            jvmTarget.set(JvmTarget.JVM_17)
        }
    }

    buildFeatures {
        compose = true
    }

    sourceSets {
        getByName("main") {
            assets.srcDirs(
                file("src/main/assets"),
                file("../../assets_gs")
            )
            java.srcDirs(
                file("src/main/java"),
                file("../../components_gs/android_shared/java")
            )
            res.srcDirs(
                file("src/main/res"),
                file("../../components_gs/android_shared/res")
            )
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.09.03")

    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.core:core-splashscreen:1.0.1")
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.6")
    implementation("com.github.mik3y:usb-serial-for-android:3.7.0")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    debugImplementation(composeBom)
    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
