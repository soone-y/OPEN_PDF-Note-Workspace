#!/usr/bin/env python3
"""
apply_utf8_bom_to_ai_docs.py

公開文書 (`introduction/*`) に対して UTF-8 BOM (utf-8-sig) を付与します。
これにより、ブラウザで人間が .md / .xml / .txt の URL を直接開いた場合でも、
ブラウザが Shift-JIS / CP932 と誤認せず、100% 完璧な日本語テキストとして表示されます。
"""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

def main():
    target_files = list((REPO_ROOT / "introduction").rglob("*"))
    count = 0
    for f in target_files:
        if f.is_file():
            text = f.read_text(encoding="utf-8").lstrip("\ufeff")
            # UTF-8 BOM 付きで保存
            f.write_bytes(text.encode("utf-8-sig"))
            count += 1
            print(f"Applied UTF-8 BOM to: {f.relative_to(REPO_ROOT)}")

    print(f"\nSuccessfully applied UTF-8 BOM to {count} public documentation files.")

if __name__ == "__main__":
    main()
