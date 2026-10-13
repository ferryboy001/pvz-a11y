return {
    id = "pvz.a11y", name = "PvZ 无障碍辅助", version = "0.1.0",
    api_version = 3, sdk = ">=2.0.0 <3.0.0", entry = false, capabilities = { "engine_sdk" },
    native = {
        windows_x64 = "native/windows_x64/plugin.dll",
        android_arm64 = "native/android_arm64/plugin.so",
    }
}
