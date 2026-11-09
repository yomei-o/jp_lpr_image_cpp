# jp_lpr_image_cpp

日本のナンバープレートの合成画像を作る C++ のプログラム。検出器用のシーン画像（YOLO 形式の箱付き）と、
認識器用の切り抜き画像（全項目のラベル付き）を作る。

[DevelopersIO「ナンバープレートの合成画像で物体検出モデルを作る」](https://dev.classmethod.jp/articles/detect_number_plate/)
の生成器（Python）と同じ考え方で作っている。440×220 の版に 4 つの欄を描き、遠近で傾け、1280×960 の背景に
重ならないように貼り、ぼかしとノイズを加える。コードは記事から写しておらず、一から書いた。

記事の版から、次の点を広げた。

| | 記事 | このリポジトリ |
|---|---|---|
| 地名 | 15 | **138（ご当地 51 を含む）**。`data/regions.txt`（[yolo_lpr_cpp](https://github.com/yomei-o/yolo_lpr_cpp) の spec と同じ順） |
| 分類番号 | 12 通りの固定リスト | 種別ごとの頭の数字（自家用 3/5/7/1/4/6/8、事業用 1/2/3/4/5/8、軽 5/4/8）、2017 年以降の英字（A C F H K L M P X Y）、2 桁の旧番号 |
| ひらがな | 7 字 | 自家用 さ〜ろ（し へ ん を除く）、事業用 あ〜こ・を、レンタカー わ れ、軽自家用は両方 |
| 書体 | TRM フォント JB＋ヒラギノ丸ゴ | TRM フォント JB ＋ 再配布できる丸ゴシック 7 種 ＋ Windows の日本語書体 12 種。足りない字は書体ごとに借りる |
| 版 | 4 種類 | 4 種類 ＋ 図柄入り、ナンバーフレーム、ボルト、浮き出し文字、汚れ、明暗のむら |
| シーン | 背景画像に貼る | 背景画像（`--bg`）または自動生成、車体、**看板（ラベルなしの紛らわしい物）**、ガウス・箱・動きのぼかし、ノイズ、色かぶり、JPEG の劣化 |
| 出力 | SageMaker Ground Truth → YOLO | YOLO 形式、`meta.tsv`（4 隅・全項目・書体）、**認識器用の切り抜き**（alpr_jp と同じフォルダ構成と全項目ラベル） |

## ビルド

```sh
sh build.sh          # g++（mingw / Linux）、OpenMP 付き → jp_lpr_image(.exe)
```

依存は同梱の stb（`third_party/`、public domain）だけ。

## 使い方

```sh
# 検出器用: シーン 1 万枚（images/*.jpg + labels/*.txt）
./jp_lpr_image --out out/det --count 10000 --threads 8

# 認識器用: 切り抜きだけ（漢字・ひらがなの学習用）
./jp_lpr_image --out out/ocr --count 20000 --crops --no-scenes --width 0.08,0.35 --threads 8

# 背景に実写を使う、ご当地を半分にする、版の種類を YOLO のクラスにする（記事と同じ 4 クラス）
./jp_lpr_image --out out/det --count 5000 --bg backgrounds/ --gotochi-p 0.5 --classes category
```

`--help` ですべてのオプションを表示する。画像 i の乱数の種は `--seed + i` なので、`--start` を使えば
一部分だけ作り直したり、分けて生成したりできる。

## 出力

```
out/
  images/00000000.jpg          シーン（--size 既定 1280x960）
  labels/00000000.txt          YOLO: class cx cy w h（0..1）。class は 0 = plate、--classes category なら 0..3
  meta.tsv                     1 版 1 行: 種別 地名 分類番号 ひらがな 一連番号 4隅 書体
  crops/<種別>/<地名>/00000000_0.png   --crops: 版の切り抜き（シーンと同じ JPEG 劣化つき）
  crops/labels.tsv             key region cls hira num（num の . は空き桁 ・）
```

`crops/` は dyama/alpr_jp と同じフォルダ構成（`自家用/品川/…`）で、`labels.tsv` は yolo_lpr_cpp の
`spec/alpr_labels.tsv` と同じ書式。そのため、yolo_lpr_cpp の学習器にそのまま入れられる。

```sh
python tools/train_ocr.py --alpr out/ocr/crops --alpr-labels out/ocr/crops/labels.tsv ...
./jlpr.exe train --alpr out/ocr/crops --alpr-labels out/ocr/crops/labels.tsv ...
```

## 書体

[fonts/README.md](fonts/README.md) を参照。Windows では `C:\Windows\Fonts` のメイリオ・游ゴシック・
BIZ UD・UD デジタル教科書体・HG 創英角ゴシック UB なども実行時に読み込む。これらはリポジトリには入れていない。

1 つの欄（地名・分類番号・ひらがな・一連番号）は、その欄の字をすべて描ける書体から 1 つ選んで描く。
TRM フォント JB が描ける欄では、`--trm-p`（既定 0.35）の割合で TRM を選ぶ。

## ライセンス

ソースコード: BSD 3-Clause（`LICENSE`）。書体はそれぞれのライセンス（`fonts/README.md`）。
`third_party/stb_*.h` は public domain。
