# IME Unix 服務

Llavon IME 在 Linux 與 macOS 上的 Unix socket 服務，為 Linux Fcitx5 附加元件與
macOS 原生 app 提供以 session 為單位的推論。模型載入、斷詞與 llama.cpp 推論由
儲存庫根目錄的 `ime-core` 子模組提供。

## 建置

在儲存庫根目錄初始化子模組，並從建置環境或 CI 傳入 vcpkg 工具鏈：

```bash
git submodule update --init --recursive
cd ime-unix-service
cmake --preset linux \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DIME_UNIX_SERVICE_BUILD_TESTS=ON
cmake --build --preset linux
ctest --test-dir build/linux --output-on-failure
cmake --install build/linux
```

安裝輸出包含服務，以及來自 `ime-core` 的標準表（位於設定的安裝前綴之下）：

```text
bin/llavon-ime-unix-service
bin/llavon-ime-lora
bin/llavon-ime-lora-gui
share/llavon-ime/tables/
```

## 執行

明確傳入必要的模型檔與表目錄：

```bash
dist/bin/llavon-ime-unix-service \
  --model path/to/llavon-ime-llama-250m-Q4_K_M.gguf \
  --tables path/to/tables
```

服務也接受透過 `LLAVON_IME_MODEL_PATH`、`LLAVON_IME_TABLES_DIR` 與
`LLAVON_IME_UNIX_SOCKET_PATH` 環境變數設定。前端會使用已安裝的執行檔，而不是把
服務加為 CMake 子目錄。

## 選用的本機 LoRA 訓練

輸入法的「**收集個人化訓練資料**」選項預設關閉。啟用後，完成且非敏感的注音提交
會送到每位使用者自己的服務，並儲存在 Linux 的
`${XDG_STATE_HOME:-$HOME/.local/state}/llavon-ime/training/commits.sqlite3` 或
macOS 的 `~/Library/Application Support/llavon-ime/training/commits.sqlite3`。
`LLAVON_IME_TRAINING_DATABASE_PATH` 可覆寫該檔案。文字只存在本機，永遠不會送到
模型下載服務。Unix socket 與資料庫僅限目前使用者存取。可用
`llavon-ime-lora list` 檢視待處理紀錄，並用 `llavon-ime-lora exclude --id ID`
或 `delete --id ID` 在訓練前移除紀錄。刪除已訓練過的紀錄會移除其儲存的文字與
讀音，但不會回復已經訓練完成的 adapter。

輸入法選單的「**管理個人化訓練…**」動作會啟動 `llavon-ime-lora-gui`，這是一個
獨立的本機網頁介面。macOS 的設定視窗有「**使用我的輸入改進模型**」按鈕；Linux 的
同一個按鈕位於 Fcitx5 輸入法設定中。與 Windows 相同，頁面會挑選待訓練紀錄
（預設全選）、檢查或下載固定的 checkpoint、提供相同的訓練預設值
（rank/alpha 8/16、dropout 0、batch size 與 accumulation 1、5 epochs、
max steps -1、learning rate 1e-4、FP32、`q_proj,v_proj`、device `auto`），
顯示進度與執行歷史，並可取消自己的行程群組。頁面會輪詢資料庫，因此開啟期間輸入的
紀錄會自動出現並預設加入選取；開始訓練時會排除畫面上未勾選的紀錄。
無法轉換的紀錄會維持待處理。手動明確選擇候選字會貢獻三個樣本，其他紀錄則貢獻
一個。完成的 GGUF 可以直接從其歷史列選為推論模型。每筆紀錄會以驗證網頁介面的
風格渲染成附帶注音的預覽；頁面會到已安裝的字表（`bopomofo_char.json`）查讀音，
紀錄中的讀音缺少時改用字表，並標記字表未列出該字讀音的情況。
管理器會呼叫獨立的 `llavon-ime-lora` CLI。管理器只綁定 `127.0.0.1` 的隨機埠，
並使用每次啟動的存取 token。再次開啟時會沿用執行中的管理器；以較新原始碼建置的
管理器會取代閒置中的執行個體，讓瀏覽器不會停留在過舊的介面，但有工作進行中的
執行個體不受影響。頁面關閉後管理器會在閒置時結束，有訓練工作進行中則會繼續執行。
Linux 套件用 `xdg-open`、macOS 用 `/usr/bin/open` 開啟預設瀏覽器。想從終端機
啟動時，直接執行 `llavon-ime-lora-gui`（或套件中的私有執行檔路徑）。

`llavon-ime-lora` 是獨立的命令列管理器；不會在輸入法或預測服務裡執行 Torch。
GUI 的「**安裝／更新 LoRA Trainer**」動作會從
[lora-trainer](https://github.com/llavon-ime/lora-trainer) 下載該平台官方、
經 SHA-256 驗證的 CPU 發行版，安裝到使用者的訓練狀態目錄下並自動使用。
安裝的是整個發行目錄，因為 TorchSharp 會載入隨執行檔附帶的原生函式庫；只含
執行檔的壓縮檔會被拒絕，缺少那些函式庫的安裝永遠不會被視為可用。
安裝器是冪等的：已安裝的執行檔仍符合固定的 commit 與其記錄的 SHA-256 時不會
下載任何東西；已驗證的壓縮檔會快取在
`${XDG_CACHE_HOME:-$HOME/.cache}/llavon-ime/lora-trainer`，讓第二個目標或之後
的打包流程重複使用（`LLAVON_IME_LORA_TRAINER_CACHE` 可覆寫該目錄）。
與 Windows 相同，`lora-trainer` 是固定版本的原始碼子模組：其 Git commit 決定
要用的對應發行執行檔，但 app 不會從子模組建置 Torch。安裝器會等 `latest.json`
回報該 commit，然後在發佈執行檔前檢查不可變的版本化發行 manifest 與平台壓縮檔的
SHA-256。永遠不會用其他發行 commit 替代。`scripts/build-linux.sh` 會在安裝時
執行同樣的驗證下載，並把 trainer 放到
`<private library dir>/llavon-ime/tools/lora`；設定
`LLAVON_IME_SKIP_LORA_TRAINER` 可跳過。deb、RPM 與 macOS 套件會在打包時內附該
固定發行版（CI 在 release workflow 下載），因此安裝後就已帶有 trainer；GUI
動作之後會為目前使用者就地更新。開發與測試時，`LLAVON_IME_LORA_CLI_PATH` 可
覆寫執行檔，`LLAVON_IME_LORA_ASSETS_DIR` 可覆寫 checkpoint 與訓練執行根目錄，
與 Windows 服務的變數一致。管理器會檢查執行檔 `--version --json` 的結果是否為
trainer API 2，而不是信任發行 manifest 的 API 欄位。在 Debian/RPM 套件中，
管理器位於 `/usr/lib/llavon-ime/llavon-ime-lora` 或
`/usr/lib64/llavon-ime/llavon-ime-lora`；macOS 則在
`/Library/Application Support/llavon-ime/payload/bin/llavon-ime-lora`。
以下範例假設它的目錄已加入 `PATH`。

例如：

```sh
state="$HOME/.local/state/llavon-ime/training"
assets="$state/assets"
llavon-ime-lora fetch-model --output-dir "$assets"
llavon-ime-lora install-trainer --output-dir "$state/tools/lora"
revision="$(cat "$assets/current.revision")"
model_dir="$assets/$revision"

llavon-ime-lora list
llavon-ime-lora dataset --model-dir "$model_dir" \
  --tables-dir /path/to/installed/share/llavon-ime/tables \
  --output "$assets/review.jsonl"
llavon-ime-lora train --model-dir "$model_dir" \
  --tables-dir /path/to/installed/share/llavon-ime/tables \
  --revision "$revision" \
  --output-dir "$state/runs/first"
```

管理器會檢查 checkpoint 版本、模型詞彙與相容的表格式，然後在開始前用 trainer
驗證數值 JSONL。目前的推論表比公開訓練 checkpoint 的詞彙多出兩個 token ID；
管理器會把已知的新增項目投影回與 checkpoint 相容的表，並讓不支援的提交維持
待處理。訓練預設為 auto/float32。`--device` 可選 `auto`、`cpu`、`cuda` 與
`mps`：`auto` 會依序選擇 CUDA（含 ROCm）、Apple Silicon 的 Metal 與 CPU，
`cuda` 需要支援 CUDA 或 ROCm 的 trainer，`mps` 需要 macOS 的 Metal 版
trainer。要用自己建置的 trainer（例如 ROCm 版本）時，把
`LLAVON_IME_LORA_CLI_PATH` 指向該執行檔即可。
每次執行會寫出一個 adapter 與一個 Q4_K_M GGUF 模型。與 Windows 相同，之後的
執行在 rank、alpha、dropout 與 target modules 相符時會從最後一個 adapter 繼續，
並沿用該 adapter 記錄的基礎 checkpoint 版本，而不是剛傳入的目錄。訓練歷史會記錄
父執行、累計紀錄數、optimizer 步數與 LoRA 參數。紀錄只會在模型匯出後標記為已
訓練；在 GUI 歷史中選用產生的 GGUF 之前，原本的推論模型會保持使用中。Linux 會
重新載入 Fcitx5 設定並為新模型重建推論傳輸；macOS 會收到本機通知、重新讀取已
儲存的設定並重新啟動預測服務。
