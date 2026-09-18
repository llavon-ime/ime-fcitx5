# Llavon IME Fcitx5 前端

Llavon IME 的 Linux 與 macOS Fcitx5 前端。推論由 `ime-unix-service` 子模組以
獨立處理程序 `llavon-ime-unix-service` 執行，透過 Unix socket 與附加元件通訊。

## 安裝

### Linux

```bash
./scripts/build-linux.sh
```

腳本會初始化子模組與 vcpkg、編譯並測試服務與附加元件，再安裝（必要時使用
`sudo`）。模型已有就沿用，否則從 Hugging Face 下載到 `models/` 並安裝至
`/usr/share/llavon-ime/models/`；可用 `IME_FCITX5_MODEL_URL` 指定映像站、
`IME_FCITX5_MODEL_DIR` 變更下載目錄。目前僅支援 x86_64。

需要 CMake、pkg-config 與 fcitx5 開發檔案。

`v*` tag 會在 GitHub Release 產生 x86_64 套件，內含模型、Vulkan backend 與
依 CPU 自動選擇的 ggml CPU backend：

```bash
# Debian / Ubuntu
sudo apt install ./llavon-ime-fcitx5_<版本>_amd64.deb

# Fedora
sudo dnf install ./llavon-ime-fcitx5-<版本>-1.<fedora>.x86_64.rpm
```

deb 以 Debian 13 建置，需 Fcitx5 5.1.12 與 glibc 2.41 以上（如 Debian 13、
Ubuntu 26.04）。

### macOS

```bash
brew tap llavon-ime/llavon-ime
brew trust --cask llavon-ime/llavon-ime/llavon-ime
brew install --cask llavon-ime
```

安裝過程會要求管理員密碼，會一併安裝小企鵝輸入法（Fcitx5）、拉風輸入法與模型，
並自動啟用輸入法及加入 macOS 輸入來源。首次安裝需登出再登入（或重開機），
候選窗才能在全螢幕應用程式中顯示。

也可以從 [Releases](https://github.com/llavon-ime/ime-fcitx5/releases/latest)
下載 `llavon-ime-<版本>-arm64.pkg` 安裝；未簽名，若被 Gatekeeper 阻擋請右鍵
選擇「打開」。

開發者可直接從原始碼建置：

```bash
./scripts/build-macos.sh
```

需先安裝 fcitx5-macos（Fcitx5.app 0.3.4 以上）。腳本會以目前的 checkout 編譯、
測試並安裝到 `~/Library/fcitx5`；fcitx5-macos 標頭會自動 pull 到
`$TMPDIR/llavon-ime-fcitx5-macos`，也可用 `FCITX5_MACOS_SOURCE_DIR` 指定。
模型位於 `/Library/Application Support/llavon-ime/models`，已存在就沿用，否則
下載後以 `sudo` 安裝。目前僅支援 Apple Silicon。

### 啟用

在 fcitx5 設定工具啟用 `llavon-ime`（首次安裝會自動啟用），Linux 執行
`fcitx5 -r` 重新啟動；macOS 執行：

```bash
pkill -x Fcitx5; open -gj -b org.fcitx.inputmethod.Fcitx5
```

附加元件會在需要時啟動 `llavon-ime-unix-service`。

## 強制替代詞彙

組字完成後，用 `Shift`＋左右方向鍵選取要儲存的範圍（`Ctrl+Shift`＋方向鍵亦可），
再按 `Enter` 加入強制替代詞彙。加入後會繼續組字、不會送出，按 `Esc` 只會取消選取。
選取範圍需為 2 至 8 個字，且每個字都要有對應的注音。選取時候選視窗會顯示目前
選到的文字與可用的操作（macOS 前端只能以候選視窗顯示提示；Linux 前端另外會標示底線）。

之後只要注音序列完全相同，就直接輸出指定的文字，覆蓋模型與候選字排序。
這是強制覆寫而非詞庫：手動選擇其他候選字仍會優先，該次輸入不會被取代。

詞彙以 UTF-8 純文字儲存在 `~/.config/llavon-ime/phrase_overrides.txt`，每行格式與
McBopomofo 的使用者詞彙檔相容，例如：

```text
歐陽芷珩 ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ
```

若有設定 `XDG_CONFIG_HOME`，檔案會改放在 `$XDG_CONFIG_HOME/llavon-ime/`。
也可在 Fcitx5 的 Llavon IME 設定中按「管理強制替代詞彙」增刪；替代文字與注音分欄編輯，
注音的音節之間用空白或 `-` 分隔即可（一聲就是不打任何調號），存檔後格式與檔案相同。

<details>
<summary>手動編譯</summary>

兩組 preset 都使用此儲存庫的 vcpkg 工具鏈。Linux 安裝至 `/usr` 並啟用 Vulkan
（`llama-vulkan`）；macOS 安裝至 `$HOME/Library/fcitx5` 並啟用 Metal
（`llama-metal`）。

Linux:

```bash
git clone --recurse-submodules https://github.com/llavon-ime/ime-fcitx5.git
cd ime-fcitx5
./vcpkg/bootstrap-vcpkg.sh

cd ime-unix-service
cmake --preset linux -DIME_UNIX_SERVICE_BUILD_TESTS=ON
cmake --build --preset linux
ctest --test-dir build/linux --output-on-failure
sudo cmake --install build/linux
cd ..

cd fcitx5
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
cd ..
sudo cmake --install build/fcitx5
```

macOS（需 fcitx5-macos 原始碼以取得標頭）：

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

</details>

## 模型

發行套件內含 Q4 GGUF 模型（CC BY-NC 4.0，僅限非商業用途；署名與相依套件授權
隨套件附上）。開發版本需自備模型，透過 fcitx5 設定頁面或
`IME_FCITX5_MODEL_PATH` 指定：

https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF

## 預測上下文

- Linux：透過 AT-SPI 取得游標前文字。
- macOS：透過 Fcitx5.app 的 InputMethodKit client 取得游標附近文字，不使用
  Accessibility API，也不需要「輔助使用」權限（需 Fcitx5.app 0.3.4 以上）。
