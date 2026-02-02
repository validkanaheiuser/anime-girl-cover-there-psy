// Top-level build file where you can add configuration options common to all sub-projects/modules.
plugins {
    alias(libs.plugins.android.application) apply false
}

tasks.register("copyZygiskFiles") {
    doLast {
        val moduleFolder = project.rootDir.resolve("module")

        val zygiskModule = project.project(":zygisk")
        val zygiskBuildDir = zygiskModule.layout.buildDirectory.get().asFile

        val zygiskSoDir = zygiskBuildDir
            .resolve("intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib")

        zygiskSoDir.walk()
            .filter { it.isFile && it.name == "libmockgps.so" }
            .forEach { soFile ->
                val abiFolder = soFile.parentFile.name
                val destination = moduleFolder.resolve("zygisk/$abiFolder.so")
                soFile.copyTo(destination, overwrite = true)
            }
    }
}

tasks.register<Zip>("zip") {
    dependsOn("copyZygiskFiles")

    archiveFileName.set("MockGPS.zip")
    destinationDirectory.set(project.rootDir.resolve("out"))

    from(project.rootDir.resolve("module"))
}
