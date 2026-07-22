#!/usr/bin/env python3
from __future__ import annotations

import json
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk

import analyze_repo


class CodeMetricsApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("code_metrics")
        self.root.geometry("1200x760")

        self.root_var = tk.StringVar(value=str(Path(__file__).resolve().parents[2]))
        self.scope_var = tk.StringVar(value="own")
        self.format_var = tk.StringVar(value="text")
        self.top_files_var = tk.StringVar(value="10")
        self.top_dirs_var = tk.StringVar(value="10")
        self.max_depth_var = tk.StringVar(value="3")
        self.include_var = tk.StringVar(value="")
        self.exclude_var = tk.StringVar(value="")
        self.status_var = tk.StringVar(value="Ready")
        self._is_running = False

        self._build_ui()

    def _build_ui(self) -> None:
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)

        controls = ttk.Frame(self.root, padding=12)
        controls.grid(row=0, column=0, sticky="ew")
        for col in range(7):
            controls.columnconfigure(col, weight=1 if col in {1, 3, 5} else 0)

        ttk.Label(controls, text="Root").grid(row=0, column=0, sticky="w")
        ttk.Entry(controls, textvariable=self.root_var).grid(row=0, column=1, columnspan=5, sticky="ew", padx=(6, 6))
        ttk.Button(controls, text="Browse", command=self._browse_root).grid(row=0, column=6, sticky="ew")

        ttk.Label(controls, text="Scope").grid(row=1, column=0, sticky="w", pady=(8, 0))
        ttk.Combobox(controls, textvariable=self.scope_var, values=("own", "all"), state="readonly", width=10).grid(
            row=1, column=1, sticky="w", padx=(6, 16), pady=(8, 0)
        )
        ttk.Label(controls, text="Format").grid(row=1, column=2, sticky="w", pady=(8, 0))
        ttk.Combobox(controls, textvariable=self.format_var, values=("text", "json"), state="readonly", width=10).grid(
            row=1, column=3, sticky="w", padx=(6, 16), pady=(8, 0)
        )
        ttk.Label(controls, text="Top Files").grid(row=1, column=4, sticky="w", pady=(8, 0))
        ttk.Entry(controls, textvariable=self.top_files_var, width=8).grid(row=1, column=5, sticky="w", padx=(6, 16), pady=(8, 0))
        ttk.Button(controls, text="Analyze", command=self.run_analysis).grid(row=1, column=6, sticky="ew", pady=(8, 0))

        ttk.Label(controls, text="Top Dirs").grid(row=2, column=0, sticky="w", pady=(8, 0))
        ttk.Entry(controls, textvariable=self.top_dirs_var, width=8).grid(row=2, column=1, sticky="w", padx=(6, 16), pady=(8, 0))
        ttk.Label(controls, text="Tree Depth").grid(row=2, column=2, sticky="w", pady=(8, 0))
        ttk.Entry(controls, textvariable=self.max_depth_var, width=8).grid(row=2, column=3, sticky="w", padx=(6, 16), pady=(8, 0))
        ttk.Label(controls, text="Include").grid(row=2, column=4, sticky="w", pady=(8, 0))
        ttk.Entry(controls, textvariable=self.include_var).grid(row=2, column=5, sticky="ew", padx=(6, 16), pady=(8, 0))
        ttk.Button(controls, text="Save Output", command=self._save_output).grid(row=2, column=6, sticky="ew", pady=(8, 0))

        ttk.Label(controls, text="Exclude").grid(row=3, column=0, sticky="w", pady=(8, 0))
        ttk.Entry(controls, textvariable=self.exclude_var).grid(row=3, column=1, columnspan=5, sticky="ew", padx=(6, 6), pady=(8, 0))
        ttk.Label(controls, textvariable=self.status_var).grid(row=3, column=6, sticky="e", pady=(8, 0))

        result_frame = ttk.Frame(self.root, padding=(12, 0, 12, 12))
        result_frame.grid(row=1, column=0, sticky="nsew")
        result_frame.columnconfigure(0, weight=1)
        result_frame.rowconfigure(0, weight=1)

        self.output = tk.Text(result_frame, wrap="none", undo=False)
        self.output.grid(row=0, column=0, sticky="nsew")
        self.output.configure(font=("Consolas", 10))

        y_scroll = ttk.Scrollbar(result_frame, orient="vertical", command=self.output.yview)
        y_scroll.grid(row=0, column=1, sticky="ns")
        x_scroll = ttk.Scrollbar(result_frame, orient="horizontal", command=self.output.xview)
        x_scroll.grid(row=1, column=0, sticky="ew")
        self.output.configure(yscrollcommand=y_scroll.set, xscrollcommand=x_scroll.set)

    def _browse_root(self) -> None:
        selected = filedialog.askdirectory(initialdir=self.root_var.get() or str(Path.cwd()))
        if selected:
            self.root_var.set(selected)

    def _parse_csv_field(self, value: str) -> list[str]:
        return [part.strip() for part in value.split(",") if part.strip()]

    def _get_positive_int(self, value: str, fallback: int) -> int:
        try:
            parsed = int(value)
            return parsed if parsed > 0 else fallback
        except ValueError:
            return fallback

    def run_analysis(self) -> None:
        if self._is_running:
            return
        self._is_running = True
        self.status_var.set("Analyzing...")
        self.output.delete("1.0", "end")
        self.output.insert("1.0", "Running analysis...\n")

        worker = threading.Thread(target=self._run_analysis_worker, daemon=True)
        worker.start()

    def _run_analysis_worker(self) -> None:
        try:
            root = Path(self.root_var.get()).resolve()
            data = analyze_repo.analyze_repository(
                root=root,
                scope=self.scope_var.get(),
                include=self._parse_csv_field(self.include_var.get()),
                exclude=self._parse_csv_field(self.exclude_var.get()),
            )
            if self.format_var.get() == "json":
                rendered = json.dumps(data, ensure_ascii=False, indent=2)
            else:
                rendered = analyze_repo.render_text_report(
                    data,
                    self._get_positive_int(self.top_files_var.get(), 10),
                    self._get_positive_int(self.top_dirs_var.get(), 10),
                    self._get_positive_int(self.max_depth_var.get(), 3),
                )
            self.root.after(0, self._show_result, rendered)
        except Exception as exc:
            self.root.after(0, self._show_result, f"Error:\n{exc}")

    def _show_result(self, rendered: str) -> None:
        self.output.delete("1.0", "end")
        self.output.insert("1.0", rendered)
        self.status_var.set("Ready")
        self._is_running = False

    def _save_output(self) -> None:
        content = self.output.get("1.0", "end-1c")
        if not content.strip():
            self.status_var.set("Nothing to save")
            return
        extension = ".json" if self.format_var.get() == "json" else ".txt"
        path = filedialog.asksaveasfilename(
            defaultextension=extension,
            filetypes=[("JSON", "*.json"), ("Text", "*.txt"), ("All Files", "*.*")],
            initialfile=f"code_metrics_{self.scope_var.get()}{extension}",
        )
        if not path:
            return
        Path(path).write_text(content, encoding="utf-8")
        self.status_var.set(f"Saved: {path}")


def main() -> int:
    root = tk.Tk()
    ttk.Style(root).theme_use("clam")
    CodeMetricsApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
