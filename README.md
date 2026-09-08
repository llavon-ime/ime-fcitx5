# Llavon IME Fcitx5 前端

Llavon IME 的 Linux 與 macOS Fcitx5 前端。推論功能由 `ime-unix-service`
子模組以獨立的 `llavon-ime-unix-service` 處理程序執行，並透過以工作階段為基礎的
Unix socket IPC 通訊。此服務使用其內嵌的 `ime-core` 子模組載入模型、進行
詞元化，以及執行 llama.cpp 推論。

## Linux 編譯

請先安裝 CMake、pkg-config 與 fcitx5 開發檔案。

執行以下單一指令即可初始化相依套件、編譯並測試 Unix 服務與 Fcitx5
附加元件，然後完成安裝：

```bash
./scripts/build-linux.sh
```

此腳本也會從 Hugging Face 下載預設的 GGUF 模型至 `models/`，再將其安裝至
`/usr/share/llavon-ime/models/`。既有且非空的模型檔案會直接沿用。如果
腳本不是以 root 身分執行，安裝指令會使用 `sudo`。可設定
`IME_FCITX5_MODEL_URL` 使用映像站，或設定 `IME_FCITX5_MODEL_DIR` 變更下載
目錄。

對應的手動操作步驟如下：

```bash
git clone --recurse-submodules https://github.com/llavon-ime/ime-fcitx5.git
cd ime-fcitx5
./vcpkg/bootstrap-vcpkg.sh

cd ime-unix-service
cmake --preset linux -DIME_UNIX_SERVICE_BUILD_TESTS=ON
cmake --build --preset linux
ctest --test-dir build/linux --output-on-failure
cd ..
sudo cmake --install ime-unix-service/build/linux

cd fcitx5
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
cd ..
sudo cmake --install build/fcitx5
```

每個專案各自定義 CMake 預設組態：

- `ime-unix-service/CMakePresets.json`：服務會安裝至 `/usr`（Linux）或
  `$HOME/Library/fcitx5`（macOS），透過 `llama-vulkan`（Linux）或
  `llama-metal`（macOS）資訊清單功能啟用 GPU 卸載，並使用
  `x64-linux-llavon` 或 `arm64-osx-llavon` vcpkg 三元組，為內附的
  llama.cpp/ggml 啟用 `GGML_VULKAN=ON` 與 `GGML_NATIVE=ON`。
- `fcitx5/CMakePresets.json`：附加元件與 AUR 套件相同，安裝至 `/usr`。

兩組預設組態會使用此儲存庫的 vcpkg 工具鏈，且不固定產生器。CMake
會選用預設值（例如 Ninja 或 Unix Makefiles），也可以明確傳入 `-G Ninja`
或 `-G "Unix Makefiles"`。

請在 fcitx5 設定工具中啟用 `llavon-ime`，再執行 `fcitx5 -r` 重新啟動
fcitx5。附加元件會在需要時啟動 `llavon-ime-unix-service`。

## macOS 編譯

請安裝 fcitx5-macos，並複製其原始碼以取得標頭檔，接著編譯服務與附加
元件。請將 `FCITX5_MACOS_SOURCE_DIR` 設為 fcitx5-macos 原始碼目錄；尋找
模組會讀取此環境變數：

```bash
export FCITX5_MACOS_SOURCE_DIR=/path/to/fcitx5-macos

cd ime-unix-service
cmake --preset macos
cmake --build --preset macos
cmake --install build/macos
cd ..

cd fcitx5
cmake --preset macos
cmake --build --preset macos
ctest --preset macos
cd ..
cmake --install build/macos
```

在 macOS 上，預設組態會安裝至 `$HOME/Library/fcitx5`，發行套件的
`postinstall` 腳本也會將內容檔案複製到此目錄。`arm64-osx-llavon` vcpkg
三元組會啟用 `GGML_NATIVE=ON`，而預設組態會透過 `llama-metal` 資訊清單
功能選用 Metal 後端。

## 模型

發行套件包含 Q4 GGUF 模型。開發版本需要本機模型，請透過 fcitx5 設定
頁面或 `IME_FCITX5_MODEL_PATH` 指定。內附模型採用 CC BY-NC 4.0 授權，僅限
非商業用途。發行套件中包含其署名聲明與軟體相依套件授權。

https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF
