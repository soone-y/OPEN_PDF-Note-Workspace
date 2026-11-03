"""Search cancellation wiring; executable checks live in search_cancel_app_smoke.py."""
from pathlib import Path
import json
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SearchCancelTests(unittest.TestCase):
    def test_filter_covers_all_ui_thread_message_loops(self):
        text = (ROOT / "src/search/search.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("SetWindowsHookExW(WH_GETMESSAGE, SearchMessageHook, nullptr, GetCurrentThreadId())", text)
        self.assertIn("if (!ctx->messageHook) return -1", text)
        self.assertIn("UnhookWindowsHookEx(ctx->messageHook)", text)
        self.assertIn("action == PM_REMOVE", text)
        self.assertIn("if (HandleSearchMessage(message)) message.message = WM_NULL", text)
        self.assertIn("CallNextHookEx(nullptr, code, action, data)", text)

    def test_scoped_filter_reuses_preserving_cancellation(self):
        text = (ROOT / "src/search/search.cpp").read_text(encoding="utf-8-sig")
        body = text.split("bool HandleSearchMessage(const MSG& message) {", 1)[1].split("static LRESULT CALLBACK SearchMessageHook", 1)[0]
        for guard in ("message.message != WM_KEYDOWN", "message.wParam != L'C'",
                      "VK_CONTROL", "VK_MENU", "VK_SHIFT", "!IsWindowEnabled(window)",
                      "!IsChild(window, message.hwnd)", "!IsChild(window, focus)", "!ctx->job.active"):
            self.assertIn(guard, body)
        self.assertIn("CancelSearchJob(window, ctx, false)", body)
        self.assertNotIn("DestroyWindow", body)
        cancel = text.split("static void CancelSearchJob(HWND hWnd, SearchCtx* ctx, bool silent) {", 1)[1].split("static std::filesystem", 1)[0]
        self.assertIn("KillTimer(hWnd, kSearchTimerId)", cancel)
        self.assertIn("SetSearchControlsEnabled(ctx, true)", cancel)
        self.assertNotIn("ResetSearchState", cancel)
        self.assertNotIn("LB_RESETCONTENT", cancel)
        self.assertIn("SetFocus(ctx->btnRun)", text)

    def test_localized_hint_and_help(self):
        for locale, label in (("ja", "中止 (Ctrl+C)"), ("en", "Stop (Ctrl+C)")):
            catalog = json.loads((ROOT / f"locales/{locale}.json").read_text(encoding="utf-8-sig"))
            self.assertEqual(catalog["search.ui.cancel"], label)
            self.assertNotIn("Ctrl+C", catalog["search.ui.run"])
            self.assertIn(label, (ROOT / f"docs/{locale}/Help_Reference.md").read_text(encoding="utf-8-sig"))


if __name__ == "__main__":
    unittest.main()
