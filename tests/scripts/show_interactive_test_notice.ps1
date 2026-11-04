# A visible stop control for both isolated and input-desktop tests.
param(
    [Parameter(Mandatory)][string]$EventPrefix,
    [Parameter(Mandatory)][int]$ParentProcessId,
    [Parameter(Mandatory)][string]$TestName,
    [ValidateSet('isolated','interactive')][string]$ExecutionMode = 'interactive',
    [Parameter(Mandatory)][string]$RecordPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
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
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern IntPtr OpenJobObject(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool TerminateJobObject(IntPtr job, uint exitCode);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool QueryInformationJobObject(IntPtr job, int kind, ref JobAccounting info, uint size, IntPtr length);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool TerminateProcess(IntPtr process, uint exitCode);
    [DllImport("dwmapi.dll")] private static extern int DwmSetWindowAttribute(IntPtr window, int attr, ref int value, int size);
    [StructLayout(LayoutKind.Sequential)]
    private struct JobAccounting {
        public long User, Kernel, PeriodUser, PeriodKernel;
        public uint Faults, Total, Active, Terminated;
    }
    public bool Completed;
    private readonly System.Windows.Forms.Timer clock = new System.Windows.Forms.Timer();
    private readonly Stopwatch elapsed = new Stopwatch();
    private readonly Label status = new Label();
    private readonly Label details = new Label();
    private readonly Button stop = new Button();
    private readonly EventWaitHandle ready, cancel, finished, running, failed, interrupted, cleanupFailed, ownerPulse, uiPulse;
    private readonly Process parent;
    private readonly string prefix, testName, mode, recordPath;
    private IntPtr testJob;
    private bool cancellation, graceComplete, allowClose, terminal, terminalConfirmed, emergency;
    private long lastOwnerMs, terminalMs, emergencyMs;
    private int ActiveCount() {
        if (testJob == IntPtr.Zero) return -1;
        JobAccounting account = new JobAccounting();
        if (!QueryInformationJobObject(testJob, 1, ref account, (uint)Marshal.SizeOf(account), IntPtr.Zero)) return -1;
        return checked((int)account.Active);
    }
    private void RequestCancellation() {
        if (terminal) { allowClose = true; Close(); return; }
        if (cancellation) return;
        cancellation = true;
        cancel.Set();
        stop.Enabled = false;
        Console.WriteLine("[CONTROL] interruption requested; waiting for owned test cleanup.");
        UpdateState();
    }
    private void EmergencyStop() {
        if (!emergency) {
            emergency = true; emergencyMs = elapsed.ElapsedMilliseconds;
            cancel.Set();
            Console.Error.WriteLine("[CONTROL] supervisor heartbeat lost; independent owned-job stop.");
            // Open only the unpredictable job name assigned to this test, never
            // enumerate/close unrelated application windows or kill by EXE name.
            if (testJob != IntPtr.Zero && !TerminateJobObject(testJob, 130))
                Console.Error.WriteLine("[CONTROL] independent termination error=" + Marshal.GetLastWin32Error());
        }
        int active = ActiveCount();
        if (active == 0 || elapsed.ElapsedMilliseconds - emergencyMs > 5000) {
            bool confirmed = active == 0;
            string evidence = "{\"format\":\"test-control-emergency-v1\",\"cleanup_confirmed\":" +
                (confirmed ? "true" : "false") + ",\"owned_active\":" + active + "}";
            try {
                string target = Path.Combine(Path.GetDirectoryName(recordPath), "emergency.json");
                string pending = target + ".pending";
                using (FileStream stream = new FileStream(pending, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
                    byte[] bytes = Encoding.UTF8.GetBytes(evidence + "\n");
                    stream.Write(bytes, 0, bytes.Length); stream.Flush(true);
                }
                File.Move(pending, target);
            } catch (Exception error) {
                Console.Error.WriteLine("[CONTROL] emergency record failed: " + error.Message);
            }
            Console.Error.WriteLine("[CONTROL] emergency_owned_active=" + active + "; cleanup_confirmed=" + confirmed);
            // Exact retained launcher handle. Closing its jobs also removes the
            // control window; exit 130 preserves a user's interruption upstream.
            if (!TerminateProcess(parent.Handle, cancellation ? 130u : 3u))
                Console.Error.WriteLine("[CONTROL] launcher termination failed=" + Marshal.GetLastWin32Error());
        }
    }
    private void UpdateState() {
        uiPulse.Set();
        if (ownerPulse.WaitOne(0)) lastOwnerMs = elapsed.ElapsedMilliseconds;
        bool done = finished.WaitOne(0);
        bool started = running.WaitOne(0);
        if (!done && started && testJob == IntPtr.Zero)
            testJob = OpenJobObject(0xC, false, prefix + "_tests"); // QUERY | TERMINATE
        int active = ActiveCount();
        string location = mode == "isolated" ? "別デスクトップで実行（画面の切替なし）" :
            "通常画面で実行：キーボード・マウス操作を一時中断してください。";
        if (done) {
            if (!terminal) {
                terminal = true; terminalMs = elapsed.ElapsedMilliseconds;
                terminalConfirmed = !cleanupFailed.WaitOne(0) && active <= 0;
                Completed = terminalConfirmed;
                stop.Text = "閉じる"; stop.Enabled = true;
                Console.WriteLine("[CONTROL] terminal; cleanup_confirmed=" + terminalConfirmed + "; owned_active=" + active);
            }
            status.Text = terminalConfirmed ?
                (interrupted.WaitOne(0) ? "中断しました。所有する検査プロセスの停止を確認しました。" :
                 failed.WaitOne(0) ? "検査は失敗しました。所有する検査プロセスの終了を確認しました。" :
                 "検査が完了しました。所有する検査プロセスの終了を確認しました。") :
                "終了未確認です。成功として扱わず、記録を確認してください。";
            details.Text = "検査: " + testName + "\n" + location +
                "\n検査制御ではデータを削除しません。既に行われた書き込みの巻き戻しを保証するものではありません。";
            if (elapsed.ElapsedMilliseconds - terminalMs >= (terminalConfirmed ? 2000 : 5000)) {
                allowClose = true; Close();
            }
            return;
        }
        if (parent.HasExited) { allowClose = true; Close(); return; }
        if (graceComplete && elapsed.ElapsedMilliseconds - lastOwnerMs > 5000) {
            status.Text = "監視元の応答がありません。所有する検査を独立して停止しています。";
            EmergencyStop();
            return;
        }
        int seconds = Math.Max(0, (int)Math.Ceiling(5 - elapsed.Elapsed.TotalSeconds));
        if (!cancellation && seconds == 0 && !graceComplete) {
            graceComplete = true;
            Console.WriteLine("[NOTICE] grace_elapsed_ms=" + elapsed.ElapsedMilliseconds);
            ready.Set();
        }
        status.Text = cancellation ? "中断処理中：プロセスの停止確認が終わるまでお待ちください。" :
            started ? "検査を実行中です。「中断」で停止できます。" :
            graceComplete ? "検査を開始しています。" : seconds + "秒後に検査を開始します。「中断」で取り消せます。";
        details.Text = "検査: " + testName + "\n" + location +
            "\n経過: " + (int)elapsed.Elapsed.TotalSeconds + "秒　所有プロセス: " +
            (active >= 0 ? active.ToString() : started ? "確認中" : "未起動") +
            "\nこの画面は検査の監視状態を表示します。検査内容の進捗は個別ログで確認します。";
    }
    public AppTestNotice(string eventPrefix, int parentId, string name, string executionMode, string resultPath) {
        prefix = eventPrefix; testName = name; mode = executionMode; recordPath = resultPath;
        ready = EventWaitHandle.OpenExisting(prefix + "_ready");
        cancel = EventWaitHandle.OpenExisting(prefix + "_cancel");
        finished = EventWaitHandle.OpenExisting(prefix + "_finished");
        running = EventWaitHandle.OpenExisting(prefix + "_running");
        failed = EventWaitHandle.OpenExisting(prefix + "_failed");
        interrupted = EventWaitHandle.OpenExisting(prefix + "_interrupted");
        cleanupFailed = EventWaitHandle.OpenExisting(prefix + "_cleanup_failed");
        ownerPulse = EventWaitHandle.OpenExisting(prefix + "_owner_pulse");
        uiPulse = EventWaitHandle.OpenExisting(prefix + "_ui_pulse");
        parent = Process.GetProcessById(parentId);
        IntPtr parentHandle = parent.Handle; // Retain exact identity across PID reuse.
        Text = "検査コントロール";
        ClientSize = new Size(760, 310);
        AutoScaleMode = AutoScaleMode.Font;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MinimizeBox = false; MaximizeBox = false; ControlBox = true;
        ShowInTaskbar = true; TopMost = true;
        StartPosition = FormStartPosition.Manual;
        Rectangle area = Screen.FromHandle(GetForegroundWindow()).WorkingArea;
        Location = new Point(Math.Max(area.Left, area.Right - Width - 16), area.Top + 16);
        Font = SystemFonts.MessageBoxFont;
        TableLayoutPanel layout = new TableLayoutPanel();
        layout.Dock = DockStyle.Fill; layout.Padding = new Padding(16); layout.ColumnCount = 1; layout.RowCount = 4;
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 46));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 60));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
        status.Dock = DockStyle.Fill;
        details.Dock = DockStyle.Fill;
        TextBox record = new TextBox();
        record.Multiline = true; record.ReadOnly = true; record.Dock = DockStyle.Fill;
        record.Text = "終了確認・診断記録（制御画面は検査データを削除しません）:\r\n" + recordPath;
        record.TabStop = true;
        stop.Text = "中断"; stop.Name = "InterruptTest"; stop.Size = new Size(110, 30); stop.Anchor = AnchorStyles.Right;
        stop.Click += delegate { RequestCancellation(); };
        layout.Controls.Add(status); layout.Controls.Add(details); layout.Controls.Add(record); layout.Controls.Add(stop);
        Controls.Add(layout);
        clock.Interval = 100;
        clock.Tick += delegate {
            try { UpdateState(); }
            catch (Exception error) {
                Console.Error.WriteLine("[CONTROL] UI monitoring failed: " + error.Message);
                cancel.Set(); allowClose = true; Close(); // No standard error dialog or sound.
            }
        };
        Shown += delegate {
            try { int corner = 1; DwmSetWindowAttribute(Handle, 33, ref corner, sizeof(int)); }
            catch (DllNotFoundException) { } catch (EntryPointNotFoundException) { }
            if (!IsWindowVisible(Handle)) {
                Console.Error.WriteLine("[CONTROL] window hidden; test must not start.");
                allowClose = true; Close(); return;
            }
            UpdateState(); Refresh();
            Console.WriteLine("[NOTICE] displayed; grace_seconds=5; persistent test control.");
            elapsed.Start(); clock.Start();
        };
        FormClosing += delegate(object sender, FormClosingEventArgs e) {
            if (!allowClose && !terminal) { e.Cancel = true; RequestCancellation(); }
        };
        FormClosed += delegate { clock.Stop(); };
    }
    protected override void Dispose(bool disposing) {
        if (disposing) {
            clock.Dispose();
            if (testJob != IntPtr.Zero) { CloseHandle(testJob); testJob = IntPtr.Zero; }
            foreach (EventWaitHandle handle in new [] { ready, cancel, finished, running, failed, interrupted, cleanupFailed, ownerPulse, uiPulse })
                if (handle != null) handle.Dispose();
            if (parent != null) parent.Dispose();
        }
        base.Dispose(disposing);
    }
}
'@
$notice = New-Object AppTestNotice($EventPrefix, $ParentProcessId, $TestName, $ExecutionMode, $RecordPath)
try {
    [Windows.Forms.Application]::Run($notice)
    if (-not $notice.Completed) {
        [Console]::Error.WriteLine('[CONTROL] control closed without confirmed cleanup; inspect the record.')
        exit 2
    }
    [Console]::WriteLine('[NOTICE] test cleanup completed; notice closed.')
} finally { $notice.Dispose() }
