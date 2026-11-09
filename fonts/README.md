# fonts/

生成器は、このフォルダ直下の `.ttf` / `.ttc` / `.otf` をすべて読む。Windows では `C:\Windows\Fonts` の日本語書体も
実行時に読み込む（`--sysfonts none` で止められる）。Windows の書体は**再配布できないので、ここには入れない**。

| ファイル | ライセンス | 出どころ | 何に効くか |
|---|---|---|---|
| `TrmFontJB.ttf` | 作者 Mr.T の許可を得て同梱（下記） | [TRMフォント（模型製作用お助けフォント）](https://dc-crafts.main.jp/trm/f/trm-font.php) Series JB ver1.2 | 実物のナンバーをトレースした書体。数字・ハイフン・・、分類番号の 0 1 2 3 8、ひらがな あいうかきくけこせを、地名 横浜 石川 山口 島根 岐阜 浜松 川口 なにわ 尾張小牧 和泉 |
| `GenSenRounded2-B.ttc` | SIL OFL 1.1（`licenses/OFL-GenSenRounded.txt`） | 源泉丸ゴシック（[PlateYOLO-JP](https://github.com/Kazuhito00/PlateYOLO-JP-Prototype) 同梱版） | 全地名・かな・数字 |
| `MPLUSRounded1c-Bold.ttf` | SIL OFL 1.1（`licenses/OFL-MPLUSRounded1c.txt`） | [Google Fonts](https://github.com/google/fonts/tree/main/ofl/mplusrounded1c) | 同上 |
| `ZenMaruGothic-Bold.ttf` | SIL OFL 1.1（`licenses/OFL-ZenMaruGothic.txt`） | [Google Fonts](https://github.com/google/fonts/tree/main/ofl/zenmarugothic) | 同上 |
| `KiwiMaru-Medium.ttf` | SIL OFL 1.1（`licenses/OFL-KiwiMaru.txt`） | [Google Fonts](https://github.com/google/fonts/tree/main/ofl/kiwimaru) | 同上 |
| `SawarabiGothic-Regular.ttf` | SIL OFL 1.1（`licenses/OFL-SawarabiGothic.txt`） | [Google Fonts](https://github.com/google/fonts/tree/main/ofl/sawarabigothic) | 同上 |
| `KosugiMaru-Regular.ttf` | Apache-2.0（`licenses/Apache-2.0.txt`）、Copyright 2010 The Kosugi Maru Project Authors | [Google Fonts](https://github.com/google/fonts/tree/main/apache/kosugimaru) | 同上 |
| `DroidSansFallbackFull.ttf` | Apache-2.0（`licenses/Apache-2.0.txt`）、© 2006-2010 Google Corp. | Android（Debian `fonts-droid-fallback`） | 漢字・かなのみ（**ASCII が無い**ので、数字は他の書体から借りる） |

## TRM フォント JB について

TRM フォントの使用条件では、フォントデータのコピー・二次配布には作者の許可が必要。このリポジトリへの同梱は
**作者の許可を得ている**。配布物の `JB1.2-readme.txt` と `JB1.2-key.txt`（Shift_JIS）を `licenses/` に、
手を加えずに置いてある。著作権は作者 Mr.T にある（Copyright(C) 2005-2018 Mr.T）。

このリポジトリから TRM フォントを取り出して、別の場所で再配布しないこと。使いたい人は配布元から入手すること。

TRM フォント JB は模型製作用の書体で、キーの割り当てが普通の書体と違う。生成器はこれに合わせて打ち分けている
（`src/jp_plate.hpp` の `encode()`）。

- 分類番号は全角数字で打つ。用意されているのは 0 1 2 3 8 だけ
- 「なにわ」はカタカナ、「尾張小牧」は全角 Ｏ、「和泉」は全角 Ｉ で打つ（組文字）
- 約 15000 の文字コードが割り当てられているが、形があるのは 51 字だけ。生成器は、輪郭のある字だけを「ある」と判定する

## Windows の書体

`C:\Windows\Fonts` にあれば読む。いずれも再配布できないので、リポジトリには入れていない。
**使うのは地名（漢字）の欄だけ**で、分類番号・ひらがな・一連番号には使わない。

meiryo, meiryob, msgothic, YuGothB, YuGothM, BIZ-UDGothicB/R, UDDigiKyokashoN-B/R,
HGRSGU（創英角ゴシックUB）, HGRGE, HGRSMP

`jp_lpr_image --list-fonts` で、見つかった書体と、それぞれが何地名を描けるかを表示する。
