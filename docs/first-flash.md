# 最初の書き込み（USB）

最初の 1 回だけ USB で書き込みます。この後のファームの更新は、Muse に頼む OTA でできます
（[stopwatch_guide.md](../muse-gadget-sdk/esp32/components/muse_diy_stopwatch/docs/stopwatch_guide.md)）。

**まだ一度も試していません。** ビルド（[firmware.yml](../.github/workflows/firmware.yml)）の実行も、実機への書き込みも未確認です。

## 3 つのファイルを書きます

このリポジトリは公開なので、読み上げ（sanoTTS-jp）のモデルと辞書は、Releases に載せません
（モデルには自分のライセンスがあり、再配布には帰属表示などの条件があります。
[LICENSE-MODEL.md](../muse-gadget-sdk/esp32/components/muse_diy_stopwatch/sanotts/upstream/LICENSE-MODEL.md)）。
公式のリリースから自分で取って、一緒に書き込みます。

| 書く場所 | ファイル | 入手先 |
|---|---|---|
| `0x0` | `muse-gadget-core.bin` | このリポジトリの [Releases](https://github.com/qnaiv/museStopWatch/releases)（または Actions の Artifact） |
| `0x830000` | `saanotts-jp-v4-int8.bin`（654,032 バイト。ファームのビルドでは `voice-v4-int8.bin` という名前で使う） | [sanoTTS-jp v1.2.0](https://github.com/ayutaz/sanoTTS-jp/releases) |
| `0x8F0000` | `k1-dict-44000-2mb.bin`（977,456 バイト） | 同じリリース |

- 書き込み先は [partitions_musestopwatch.csv](../muse-gadget-sdk/esp32/partitions_musestopwatch.csv) の `sano_model`、`sano_dict` です。
- 公式リリース v1.2.0 のファイル名は、モデルが `saanotts-jp-v4-int8.bin`、辞書が `k1-dict-44000-2mb.bin` です（2026-11-14 に、サイズが 654,032 / 977,456 バイトであることを確認）。
- モデルと辞書が無くても起動はしますが、読み上げは使えないはずです（未確認）。

## 書き込みの例（PC の esptool）

```
python -m esptool --chip esp32s3 --port <ポート> write_flash 0x0 muse-gadget-core.bin 0x830000 saanotts-jp-v4-int8.bin 0x8F0000 k1-dict-44000-2mb.bin
```

- 書く前に、出荷時のフラッシュ全体をバックアップしておくと安心です:
  `python -m esptool --chip esp32s3 --port <ポート> read_flash 0 0x1000000 backup.bin`
  （バックアップの中身はコミットしません）。
- スマホだけで書き込めるかは、未確認です。
- 書き込んだ後は、Muse アプリの開発者モードと SDK トークンで組にします。Wi-Fi と SDK トークンは、Museストップウォッチの Web 設定（設定 > QR）から入れられます。

## 公開リポジトリで気をつけること

- Wi-Fi のパスワード、SDK トークン（`mgst_…`）、署名鍵、Muse の VM の認証情報、実機のログ（SSID・MAC アドレス）、
  バックアップは、コミットしません。
- OTA の署名鍵は、リポジトリの Secret `MSW_SIGNING_KEY` にだけ置きます。
- 読み上げのモデルと辞書は、Releases・Artifact・リポジトリのどこにも載せません。
