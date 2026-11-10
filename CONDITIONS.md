# 実行の条件

| 項目 | 設定 |
|---|---|
| モデル | Claude Fable 5.2(`claude-fable-5-2`)、Claude Code 2.1.321、`--effort high` |
| 入力 | 空の作業フォルダーと [prompts/os.txt](prompts/os.txt) |
| 道具 | Claude Code の標準ツール(サブエージェント、Web 検索、シェル)。追加の MCP なし |
| 隔離 | bubblewrap。作業フォルダーの外の設定やメモは見せない |
| 環境 | gcc、binutils、nasm、GRUB(x86_64-efi)、xorriso、mtools、python3、QEMU 10.2(KVM なし)、OVMF |
| 人の介入 | 質問には答えない。コードは直さない |
| 回数 | 1回 |

実行前に決めたこと: 完了の定義9項目を制作側が QEMU で確かめて数える([checks/CHECKS.md](checks/CHECKS.md))。起動してログインできない、または通った項目が5つ未満なら「作れた」とは言わない。

決めた条件から外れた点: 当初は「1セッション・上限5時間・介入なし」の予定でしたが、アカウントの利用上限で3回止まりました。同じ作業フォルダーで、[prompts/resume-note.txt](prompts/resume-note.txt) を課題文の後ろに付けて再開しています。3回目も作業の途中で止まり、その時点のフォルダーをそのまま成果物としました。
