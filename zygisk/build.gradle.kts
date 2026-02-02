plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.mockgps.zygisk"
    compileSdk = 36
    ndkVersion = "29.0.14206865"
    buildToolsVersion = "36.1.0"

    packaging {
        resources {
            excludes += "**"
        }
    }

    defaultConfig {
        minSdk = 26
        multiDexEnabled = false

        externalNativeBuild {
            cmake {
                abiFilters(
                    "arm64-v8a",
                    "armeabi-v7a"
                )

                arguments(
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DANDROID_STL=c++_static",
                    "-DCMAKE_JOB_POOLS=compile=${Runtime.getRuntime().availableProcessors()}",
                    "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"
                )

                val commonFlags = setOf(
                    "-fvisibility=hidden",
                    "-ffunction-sections",
                    "-fdata-sections",
                    "-Os",
                    "-Wall",
                    "-Wextra"
                )

                cFlags += "-std=c23"
                cFlags += commonFlags

                cppFlags += "-std=c++17"
                cppFlags += commonFlags
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    externalNativeBuild {
        cmake {
            path("src/main/cpp/CMakeLists.txt")
            version = "3.30.5+"
        }
    }
}

afterEvaluate {
    tasks.named("assembleRelease") {
        finalizedBy(
            rootProject.tasks["copyZygiskFiles"],
            rootProject.tasks["zip"]
        )
    }
}
