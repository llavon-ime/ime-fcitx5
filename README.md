# Llavon IME

Llavon IME 的 Linux Fcitx5 前端、macOS 原生輸入法與 `ime-unix-service/` 預測服務
均在此儲存庫。推論由獨立處理程序 `llavon-ime-unix-service` 執行，透過 Unix
socket 與前端通訊；`ime-core/` 是共用的上游 Git submodule。

## 安裝

### Linux

```bash
./scripts/build-linux.sh
```

腳本會初始化子模組與 vcpkg、編譯並測試服務與附加元件，再安裝（必要時使用
`sudo`）。模型已有就沿用，否則從 Hugging Face 下載到 `models/` 並安裝至
`/usr/share/llavon-ime/models/`；可用 `LLAVON_IME_MODEL_URL` 指定映像站、
`LLAVON_IME_MODEL_DIR` 變更下載目錄。目前僅支援 x86_64。

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

安裝過程會要求管理員密碼，會安裝原生「拉風輸入法」、注音表、模型與 AI 預測
服務。安裝程式會把輸入來源註冊、啟用並重新啟動輸入法，升級後不需登出就能
繼續使用；安裝後到「系統設定 › 鍵盤 › 輸入方式」把它加入，macOS 會詢問是否
允許這個第三方輸入法。若輸入來源沒有出現，登出再登入（或重開機）即可。

也可以從 [Releases](https://github.com/llavon-ime/ime-unix/releases/latest)
下載 `llavon-ime-<版本>-arm64.pkg` 安裝；未簽名，若被 Gatekeeper 阻擋請右鍵
選擇「打開」。目前僅提供 arm64 安裝檔。

解除安裝：

```bash
brew uninstall --cask llavon-ime
```

套件內也附一支解除安裝腳本，會一併清掉舊 fcitx5-based 安裝留下的檔案
（`~/Library/fcitx5` 的 addon 與 service），移除前會先詢問是否也要刪除
Fcitx5.app（預設保留）：

```bash
sudo "/Library/Application Support/llavon-ime/uninstall.sh"
```

開發者可直接從原始碼建置原生輸入法（不需要 fcitx5-macos）：

```bash
macos/scripts/build-native-app.sh --install   # 安裝 app + service（需要 sudo）
```

前置需求：Xcode Command Line Tools、CMake，以及 vcpkg 需要的 `pkg-config`
（`brew install cmake pkg-config`）。腳本會用 vcpkg 編譯引擎、以 `swiftc` 編譯
前端、ad-hoc 簽章並安裝到 `/Library/Input Methods/LlavonIME.app`（與套件相同
位置，兩者不會互相 shadow）；安裝前會先移除 `~/Library/Input Methods/` 的舊
copy，因為同 bundle ID 的使用者層 copy 會蓋掉系統層的，也會讓套件安裝時把
bundle relocate 到家目錄。沒有 sudo 的機器可用 `--install --user` 裝到家目錄。
第一次安裝要登出再登入，讓 macOS 掃到輸入來源。改完程式重跑同一行指令即生效
（可先 `pkill -x LlavonIME`）。

`--install` 也會透過 `scripts/build-macos-service.sh` 建置、測試並安裝 AI 預測
服務：系統安裝裝到 `/Library/Application Support/llavon-ime/payload`（套件用的
路徑，app 優先讀這裡），`--user` 時裝到 `~/Library/fcitx5`。模型位於
`/Library/Application Support/llavon-ime/models`，已存在就沿用，沒有才下載。
只想更新 app 時加 `--no-service` 可跳過 service（建置 service 較久，目前僅
Apple Silicon）。

### 啟用

Linux 在 fcitx5 設定工具啟用 `llavon-ime`（首次安裝會自動啟用），執行
`fcitx5 -r` 重新啟動；macOS 則在「系統設定 › 鍵盤 › 輸入方式」或選單列輸入選單
選擇「拉風輸入法」。輸入法會在需要時啟動 `llavon-ime-unix-service`。

## 強制替代詞彙

組字完成後，用 `Shift`＋左右方向鍵選取要儲存的範圍（`Ctrl+Shift`＋方向鍵亦可），
再按 `Enter` 加入強制替代詞彙。加入後會繼續組字、不會送出，按 `Esc` 只會取消選取。
選取範圍需為 2 至 8 個字，且每個字都要有對應的注音。選取時候選視窗會顯示目前
選到的文字與可用的操作（macOS 前端只能以候選視窗顯示提示；Linux 前端另外會標示底線）。

之後組字中只要出現相同的注音序列（不必整段相同），就直接輸出指定的文字，覆蓋模型
與候選字排序。套用後即使繼續輸入後面的字，已替換的文字仍會保留，不會被模型改回；
若回頭編輯到已套用的範圍，或手動選擇其他候選字，就會解除並改回模型輸出。
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
git clone --recurse-submodules https://github.com/llavon-ime/ime-unix.git
cd ime-unix
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
`LLAVON_IME_MODEL_PATH` 指定：

https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF

舊版 `IME_FCITX5_*` 環境變數名稱仍相容（例如 `IME_FCITX5_MODEL_PATH`）。

## 預測上下文

- Linux：透過 AT-SPI 取得游標前文字。
- macOS：透過 Fcitx5.app 的 InputMethodKit client 取得游標附近文字，不使用
  Accessibility API，也不需要「輔助使用」權限（需 Fcitx5.app 0.3.4 以上）。
