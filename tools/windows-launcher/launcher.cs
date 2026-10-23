/* Bloodborne PC launcher (exe twin of start_lazy.bat, made for adding to Steam).
 * Sets the same environment and starts scripts/run_game.py with the portable Python.
 * The run's whole console output is teed to out\last-run.log (a run that exits nonzero
 * is copied to out\crash-<date>.log): the game's fault handler prints its stack there. */
using System;
using System.Diagnostics;
using System.IO;

class BloodbornePCLauncher {
    static readonly object logLock = new object();
    static StreamWriter log;

    static int Main(string[] args) {
        string root = AppDomain.CurrentDomain.BaseDirectory;
        string python = Path.Combine(root, "python", "python.exe");
        string game = Path.Combine(root, "Games");
        string runtime = Path.Combine(root, "runtime");

        if (!File.Exists(python)) { Wait("ERROR: portable Python not found:\n  " + python); return 1; }
        if (!File.Exists(Path.Combine(game, "eboot.bin"))) { Wait("ERROR: eboot.bin not found:\n  " + Path.Combine(game, "eboot.bin")); return 1; }
        if (!File.Exists(Path.Combine(runtime, "SDL3.dll"))) { Wait("ERROR: runtime DLLs not found:\n  " + runtime); return 1; }

        string outDir = Path.Combine(root, "out");
        string logPath = Path.Combine(outDir, "last-run.log");
        try {
            Directory.CreateDirectory(outDir);
            log = new StreamWriter(logPath, false, System.Text.Encoding.UTF8) { AutoFlush = true };
        } catch (IOException) { log = null; }

        Environment.SetEnvironmentVariable("PYTHONUTF8", "1");
        Environment.SetEnvironmentVariable("PYTHONIOENCODING", "utf-8");
        Environment.SetEnvironmentVariable("BB_PREBUILT", "1");
        /* 0.6's memory model: the GPU uses the game's direct memory in place (its chunks
         * exported to Vulkan; host memory import on Windows), as run.sh sets for the Linux
         * builds. Without it the port falls back to the 0.3 copies model, which flickers
         * with 0.6's renderer. ANY_GPU: NVIDIA too (the module, not the sparse arena). */
        Environment.SetEnvironmentVariable("BB_GUEST_IN_PLACE", "1");
        Environment.SetEnvironmentVariable("BB_PC_MODEL_ANY_GPU", "1");
        Environment.SetEnvironmentVariable("BB_DATA_DIR", root);
        Environment.SetEnvironmentVariable("BB_GAME_DIR", game);
        Environment.SetEnvironmentVariable("BB_PYTHON", python);
        Environment.SetEnvironmentVariable("BB_RUNTIME", runtime);
        Environment.SetEnvironmentVariable("PATH", runtime + ";" + Environment.GetEnvironmentVariable("PATH"));

        var psi = new ProcessStartInfo {
            FileName = python,
            Arguments = Quote(Path.Combine(root, "scripts", "run_game.py")) + " --game-dir " + Quote(game)
                        + (args.Length > 0 ? " " + string.Join(" ", Array.ConvertAll(args, Quote)) : ""),
            WorkingDirectory = root,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = System.Text.Encoding.UTF8,
            StandardErrorEncoding = System.Text.Encoding.UTF8
        };
        Line("Bloodborne PC - exe launcher");
        Line("Root:    " + root);
        Line("Game:    " + game);
        Line("Python:  " + python);
        Line("Build:   PREBUILT");
        Line("Log:     " + logPath);
        Line("Starting Bloodborne...");
        using (var p = Process.Start(psi)) {
            p.OutputDataReceived += (s, e) => Line(e.Data, false);
            p.ErrorDataReceived += (s, e) => Line(e.Data, true);
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            p.WaitForExit();
            int code = p.ExitCode;
            Line("");
            Line("Bloodborne PC exited with code " + code);
            lock (logLock) { if (log != null) log.Close(); log = null; }
            if (code != 0 && File.Exists(logPath)) {
                string crashPath = Path.Combine(outDir, "crash-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log");
                try { File.Copy(logPath, crashPath); Line2Console("Crash log: " + crashPath); } catch (IOException) { }
                Wait("");
            }
            return code;
        }
    }

    /* One child line: to the console (its stream) and to the log file. */
    static void Line(string line, bool isError = false) {
        if (line == null) return;
        if (isError) Console.Error.WriteLine(line); else Console.WriteLine(line);
        lock (logLock) { if (log != null) log.WriteLine(line); }
    }
    static void Line2Console(string line) { Console.WriteLine(line); }

    static string Quote(string s) { return "\"" + s + "\""; }
    static void Wait(string msg) {
        if (msg.Length > 0) Console.WriteLine(msg);
        Console.WriteLine("Press Enter to close...");
        try { Console.ReadLine(); } catch (IOException) { }
    }
}
