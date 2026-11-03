# Called only by isolated_app_test.py --interactive. Showing the notice itself
# must not activate a window. After five visible seconds, tests start without input.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;
public sealed class AppTestNotice : Form {
    protected override bool ShowWithoutActivation { get { return true; } }
    protected override CreateParams CreateParams {
        get { var p = base.CreateParams; p.ExStyle |= 0x08000000; return p; }
    }
    protected override void WndProc(ref Message m) {
        if (m.Msg == 0x21) { m.Result = (IntPtr)3; return; } // MA_NOACTIVATE
        base.WndProc(ref m);
    }
    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();
    public bool Completed;
    private readonly Timer clock = new Timer();
    private readonly Stopwatch elapsed = new Stopwatch();
    private readonly Label text = new Label();
    private int remaining = -1;
    private void UpdateCountdown() {
        int seconds = Math.Max(0, (int)Math.Ceiling(5 - elapsed.Elapsed.TotalSeconds));
        if (seconds == 0) { Completed = true; Close(); return; }
        if (remaining == seconds) return;
        remaining = seconds;
        text.Text = seconds + "秒後にアプリの自動検査を開始します。\n" +
            "検査中は画面・入力先・クリップボードを使用します。\n" +
            "キーボード・マウス操作を一時中断してください。\n" +
            "この通知への操作は不要です。";
    }
    public AppTestNotice() {
        Text = "App test starts soon";
        ClientSize = new Size(610, 135);
        FormBorderStyle = FormBorderStyle.FixedToolWindow;
        ControlBox = false;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.Manual;
        Rectangle area = Screen.FromHandle(GetForegroundWindow()).WorkingArea;
        Location = new Point(area.Left + Math.Max(0, (area.Width - Width) / 2),
                             area.Top + Math.Max(0, (area.Height - Height) / 2));
        TopMost = true;
        Font = SystemFonts.MessageBoxFont;
        text.SetBounds(18, 15, ClientSize.Width - 36, ClientSize.Height - 30);
        Controls.Add(text);
        clock.Interval = 100;
        clock.Tick += delegate { UpdateCountdown(); };
        Shown += delegate {
            UpdateCountdown(); Refresh();
            Console.WriteLine("[NOTICE] displayed; grace_seconds=5; no user action required.");
            elapsed.Start(); clock.Start();
        };
        FormClosed += delegate {
            clock.Stop(); clock.Dispose();
            if (Completed) Console.WriteLine("[NOTICE] grace_elapsed_ms=" + elapsed.ElapsedMilliseconds);
        };
    }
}
'@
$notice = New-Object AppTestNotice
try {
    [Windows.Forms.Application]::Run($notice)
    if (-not $notice.Completed) {
        [Console]::Error.WriteLine('[NOTICE] closed before the grace period completed; test not started.')
        exit 2
    }
    [Console]::WriteLine('[NOTICE] five-second grace period completed; starting test automatically.')
} finally { $notice.Dispose() }
