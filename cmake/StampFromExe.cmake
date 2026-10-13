# 生成 engine_sdk 构建戳：宿主指纹取游戏 exe 的实际 SHA-256，
# 而不是随包 identity.json 中的 host_fingerprint（两者在本工作区不一致，
# 直接用后者会被宿主以 "engine SDK does not match this game build" 拒绝）。
file(SHA256 "${HOST_EXE}" host_hash)
file(SHA256 "${PLUGIN}" plugin_hash)
file(WRITE "${PLUGIN}.tao-engine" "schema=1\nhost=sha256:${host_hash}\nplatform=windows_x64\nplugin=sha256:${plugin_hash}\n")
