# 結果 — Claude Fable 5.2 / 空のフォルダーから試作OS(ep525)

2026-11-10。条件は PRECHECK.md(このリポジトリでは CONDITIONS.md)、課題文は prompts/os.txt、検査項目は checks/CHECKS.md(いずれも実行前に固定)。

(このリポジトリでは、画面の抜粋を screenshots/ に置いています。本文中の frozen/ 以下のファイル名は、制作側で保管している検査時の画面と収録を指します。)

## 実行の経過

| 回 | 時刻(JST) | 長さ | サブエージェント | 終わり方 | API 定価換算 |
|---|---|---|---|---|---|
| 1 | 06:57–07:28 | 31分 | 2体(Ink、Gomoku) | 5時間枠の利用上限 | $15.02 |
| 2 | 09:41–10:01 | 20分 | 6体(エディタ、ファイル管理、Monitor+Gomoku、Ink、ネットワーク、カーネル/ウィンドウの点検) | 同上 | $36.92 |
| 3 | 14:42–15:28 | 45分 | 4体(Ink、Monitor+Gomoku、ネットワーク、利用者の立場での受け入れ試験) | 同上 | $37.37 |

- 計96分、延べ12体、換算額 約$89。モデル `claude-fable-5-2`、Claude Code、`--effort high`。
- 条件からの逸脱: PRECHECK は「介入なし・1セッション」だったが、利用上限で3回止まった。同じ作業フォルダーで、中立の再開メモ(prompts/resume-note.txt)だけを渡して再開した。1回目は同じ時間帯に別の実演2本を並走させたため上限が早く来た。
- モデルの完了報告は出ていない(3回目も作業の途中で上限)。成果物は 15:28 時点の作業フォルダーを `frozen/src` に複製し、SHA-256 を `frozen/SHA256SUMS` に記録した。以下の検査はこの複製だけを使い、コードは直していない。

## 成果物

Sumi OS 0.1 "first stroke"。C と NASM で約9,400行(kernel 3,888 / gui 1,848 / apps 3,765 / boot 79 / tools 618)。ビルド約1秒。32ビットのカーネル suzuri、ウィンドウシステム Washi、シェル fude、ファイルシステム MakiFS。アプリ9つ(Terminal、Editor、Files、Clock、Monitor、Ink、Gomoku、About、Install Sumi)。フォントは Terminus(SIL OFL 1.1、ライセンス文同梱)。

## 検査(主担当、QEMU 10.2 + OVMF、KVM なし)

QMP でキーとマウスを送り、screendump で画面を取った(checks/qmp.py、rec.py)。画面は frozen/check1/、収録は frozen/footage/。

| # | 項目 | 結果 | 根拠 |
|---|---|---|---|
| 1 | build.sh / run.sh | 確認 | 生成物を消した複製で `./build.sh` が通り、`./run.sh` で OVMF → GRUB → カーネルが起動 |
| 2 | ログイン画面 | 確認 | 1024×768。誤ったパスワードは "Incorrect user name or password"(login-wrong.png)、guest/guest で入れた(desktop.png) |
| 3 | ウィンドウ操作 | 確認 | 5枚を同時に開く、タイトルバーのドラッグで移動、クリックで前面、閉じるボタン・Alt+F4 で閉じる(w1.png、e1.png、sheet4.jpg) |
| 4 | アプリ4つ以上 | 確認 | 9つとも開いた。ターミナルは pwd, ls -l, whoami, date, uname -a, free, df, uptime, ps, echo >, cat, wc, grep, stat, ifconfig, ping の16種類に応答(sheet2.jpg) |
| 5 | 永続化 | 確認 | エディタで proof.txt(persist-check 58317-kumo)を保存 → shutdown → 再起動 → ターミナルの cat とファイル管理の一覧の両方で確認(h1.png)。memo.txt でも同じ(footage/persist.mp4) |
| 6 | 同時実行 | 確認 | ターミナルに入力している間、パネルの時計と Clock の秒針が進み、Monitor の表が更新された(tour.mp4)。入力中の2枚の画面でパネルの時計が 20:24:50 → 20:24:57(footage/conc-1.png、conc-2.png) |
| 7 | 時計 | 確認 | 画面 19:41:45、ホスト 19:41:44(JST) |
| 8 | 30分の連続操作・障害の表示 | 確認 | 30分52秒の連続操作(アプリの開閉・命令の実行を59巡。checks/soak.py)の後も操作に応答し、シリアルログに障害の記録なし(soak-end.png、serial4.log)。`crash null` → Terminal だけ閉じ「Page fault writing address 0x00000000 (NULL pointer), at c_crash+0x33」のダイアログ、他は動作継続。`crash kernel` → 停止画面に原因・レジスター・呼び出し履歴(footage/crash.mp4) |
| 9 | README | 確認(書きかけ2か所) | 構成・ビルドと起動・ログイン・操作・既知の不具合の5項目あり。既知の不具合は具体的で、検査で見つけた点と矛盾なし。差し込み用の目印 `NETCMD_ROW`(116行)と `NETDEFECTS`(341行)が残っている |

任意項目:

- インストーラー: メニューの Install Sumi で起動用の領域に書き込み、`SUMI_BOOT=disk ./run.sh`(CD イメージなし)で起動した。QEMU の起動から1024×768 のログイン画面まで 2.1 秒(footage/boot2.mp4、install.mp4)。
- ネットワーク: e1000 を認識、DHCP で 10.0.2.15 を取得、`ping 10.0.2.2` に4回応答。README のネットワークの節は未完成。
- 独自アプリ: Ink(筆の速さで線の太さが変わる。footage/ink.mp4)、Gomoku(コンピューターが応手。footage/gomoku.mp4)。

出自: ソースに他OS・libc・GUIツールキットのライセンス表記や特徴的な識別子は見つからなかった(grep)。実行ログに curl / wget / git clone によるコードの取得は見つからなかった(3回目に通信の可否を調べる curl が1回)。フォントは OFL-Terminus.txt 同梱。

## 検査で見つけた点

- README に書きかけの目印が2か所(上記)。`apps/terminal.c.orig` が残っている。
- アプリは別のメモリー空間に分かれていない(README が既知の不具合として記載)。
- 表示は Latin-1 のみ。日本語の表示・入力はない(README 記載)。
- 保存確認のダイアログを Esc で閉じるとアプリは開いたまま残る(仕様どおり)。検査スクリプト側の誤りで窓が溜まった場面があり、スクリプトを直した。OS の不具合ではない。
- 検査中に Ctrl+S が効かないように見えた場面は、検査スクリプトがキーを短い間隔で送りすぎたため。間隔を空けると Ctrl+S で保存できた。

## 判定

PRECHECK の判定式(9項目のうち主担当が確かめて通った数): 9項目とも確認(9番の README は書きかけ2か所を含む)。反証条件(起動してログインできない、または5項目未満)には当たらない。

ただし n=1、利用上限による中断3回と再開を含む。「1セッション・上限5時間で作り切る」という当初の主張の形では確かめられていない。動画では「3回に分かれた計96分でここまで動いた」と事実のまま述べる。
