using System.Diagnostics;
using Microsoft.Win32;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Threading;

namespace PwaBlocker;

internal sealed record Component(string RelativePath, string Extension, long Length, string Sha256);
internal sealed record ServiceSnapshot(bool Exists, string State, string StartType, string BinaryPath);
internal sealed record SavedServiceState(string ServiceName, string State, string StartType, string BinaryPath);

internal sealed class TeeTextWriter : TextWriter
{
    private readonly TextWriter first;
    private readonly TextWriter second;

    public TeeTextWriter(TextWriter first, TextWriter second)
    {
        this.first = first;
        this.second = second;
    }

    public override Encoding Encoding => first.Encoding;

    public override void Write(char value)
    {
        first.Write(value);
        second.Write(value);
        Flush();
    }

    public override void Write(string? value)
    {
        first.Write(value);
        second.Write(value);
        Flush();
    }

    public override void WriteLine(string? value)
    {
        first.WriteLine(value);
        second.WriteLine(value);
        Flush();
    }

    public override void Flush()
    {
        first.Flush();
        second.Flush();
    }
}

internal static class Program
{
    private const string DefaultRoot = @"D:\Program Files (x86)\perfectworldarena";
    private const string ServiceName = "MessageTransfer";
    private const string RulePrefix = "AC-Block-PWA";
    private static readonly string ServiceStatePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
        "AC", "PwaBlocker", "MessageTransfer.state.json");
    private static readonly string[] BlockedExecutableNames =
    [
        "完美世界竞技平台.exe",
        @"plugin\dota2Assist.exe",
        @"plugin\pwautil.exe",
        @"plugin\pwautil64.exe",
        @"plugin\WmgpOptimizer.exe",
        @"plugin\WmgpOptimizer_x64.exe",
        @"plugin\resource\7z\7za.exe",
        @"resources\elevate.exe"
    ];
    private static StringWriter? ActiveTranscript;
    private static string ActiveRoot = DefaultRoot;
    private static readonly object ConsoleLock = new();

    private static readonly Dictionary<string, string> ExpectedHashes = new(StringComparer.OrdinalIgnoreCase)
    {
        ["完美世界竞技平台.exe"] = "b4f006e81999de00862391e9afa014a54b8999db8f2d879a96728ce61e6ed34f",
        [@"plugin\dota2Assist.exe"] = "6d8d7c8e1bdf6a2efa96d0d326a8840b5ade84443693cfca863a21ecc868c922",
        [@"plugin\pwautil.exe"] = "5344ac57864f9c827a5a8896981118c1fdaf8d19cc9de7550f2c65526354b6f7",
        [@"plugin\pwautil64.exe"] = "f8e241803d4db41110f9d344fc3077d00d012216140632807226cd38414fe21c",
        [@"plugin\WmgpOptimizer.exe"] = "8f3cfaece2a4f4ebc4cc3b77431f1bded42990df85614327f87e792ea472c2a5",
        [@"plugin\WmgpOptimizer_x64.exe"] = "9628772845f525477e520a351a846aa5febfa7aca23ee9466272956a47ffef07",
        [@"plugin\resource\7z\7za.exe"] = "e81473caae50be17d2fab575b7fb929932793692963074559c03df7e4ac5da38",
        [@"resources\elevate.exe"] = "d1d63e9023fc7ffc4682bc5e581128af7f0da10d4f636a885cb7b7321edf08c7",
        [@"plugin\MessageTransfer.sys"] = "d584229597a7051e2542ceb8de4c987a34cd02bfb101b8bfb7585260c6fa6a67"
    };

    private static int Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        var transcript = new StringWriter();
        var tee = new TeeTextWriter(Console.Out, transcript);
        ActiveTranscript = transcript;
        Console.SetOut(tee);
        Console.SetError(tee);
        ShowLogo();
        string? root = null;
        var explicitRoot = false;
        var action = "watch";
        var allExe = false;
        var dryRun = false;
        var interval = 3;

        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i].ToLowerInvariant())
            {
                case "status": case "scan": case "block": case "unblock": case "stop": case "watch":
                    action = args[i].ToLowerInvariant(); break;
                case "--root":
                    if (++i >= args.Length) return FinishAfterUsage("--root 缺少路径");
                    root = Path.GetFullPath(args[i]);
                    explicitRoot = true;
                    ActiveRoot = root;
                    break;
                case "--all-exe": allExe = true; break;
                case "--dry-run": case "--what-if": dryRun = true; break;
                case "--interval":
                    if (++i >= args.Length || !int.TryParse(args[i], out interval) || interval is < 1 or > 3600)
                        return FinishAfterUsage("--interval 必须是 1-3600 的整数");
                    break;
                case "--help": case "-h": case "/?": return FinishAfterUsage(null);
                default: return FinishAfterUsage($"未知参数：{args[i]}");
            }
        }

        try
        {
            if (!explicitRoot)
            {
                Console.WriteLine("正在自动检测 PWA 安装路径...");
                root = DetectPwaRoot();
                if (root is null)
                {
                    root = DefaultRoot;
                    ActiveRoot = root;
                    WriteStatus("不匹配", $"未检测到有效 PWA 安装目录，当前候选：{root} | 已拦截：否");
                }
                else
                {
                    ActiveRoot = root;
                    WriteStatus("匹配", $"已自动检测目标目录：{root} | 已拦截：否");
                }
            }
            else
            {
                ActiveRoot = root!;
                WriteStatus(Directory.Exists(root!) ? "匹配" : "不匹配",
                    $"使用显式目标目录：{root} | 已拦截：否");
            }

            root ??= DefaultRoot;
            var exitCode = action switch
            {
                "status" => Status(root, allExe),
                "scan" => Scan(root),
                "block" => Block(root, allExe, dryRun),
                "unblock" => Unblock(root, dryRun),
                "stop" => Stop(root, allExe, dryRun),
                "watch" => Watch(root, allExe, dryRun, interval),
                _ => Usage($"未知动作：{action}")
            };
            if (action is "block" or "stop" or "unblock")
                PrintScreening(root, allExe);
            ShowCompletionLog(action, root, exitCode, transcript);
            if (action != "watch") WaitForManualClose();
            return exitCode;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"错误：{ex.Message}");
            ShowCompletionLog(action, root ?? DefaultRoot, 1, transcript);
            WaitForManualClose();
            return 1;
        }
    }

    private static void ShowLogo()
    {
        const string border = "+==================================================================+";
        var previousColor = Console.ForegroundColor;
        Console.ForegroundColor = ConsoleColor.DarkCyan;
        Console.WriteLine(border);
        Console.ForegroundColor = ConsoleColor.Cyan;
        Console.WriteLine("|                         G A M E S 8 T H                        |");
        Console.ForegroundColor = ConsoleColor.White;
        Console.WriteLine("|                         Games8Th.Team                          |");
        Console.ForegroundColor = ConsoleColor.Yellow;
        Console.WriteLine("|                           PWA BLOCKER                           |");
        Console.ForegroundColor = ConsoleColor.DarkGray;
        Console.WriteLine("|                 COMPONENT SCREENING CONSOLE                    |");
        Console.ForegroundColor = ConsoleColor.DarkCyan;
        Console.WriteLine(border);
        Console.WriteLine();
        Console.ForegroundColor = previousColor;
        Console.Out.Flush();
        Thread.Sleep(TimeSpan.FromSeconds(3));
    }

    private static void ShowCompletionLog(string action, string root, int exitCode, StringWriter transcript)
    {
        var directory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            "AC", "PwaBlocker", "logs");
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, $"pwa-screen-{DateTime.Now:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.log");
        var footer = $"\r\n\r\n+----------------------------------------------------------------+\r\n" +
                     "Games8Th.Team | PWA 筛查完成\r\n" +
                     $"动作：{action}\r\n" +
                     $"目标目录：{root}\r\n" +
                     $"返回码：{exitCode}\r\n" +
                     $"完成时间：{DateTime.Now:yyyy-MM-dd HH:mm:ss}\r\n" +
                     "此 CMD 日志窗口将保持打开，请手动关闭。\r\n" +
                     "+----------------------------------------------------------------+\r\n";
        File.WriteAllText(path, transcript.ToString() + footer, new UTF8Encoding(true));
        Console.WriteLine($"已生成筛查日志：{path}");
        try
        {
            var scriptPath = Path.Combine(directory, $"pwa-screen-{DateTime.Now:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.cmd");
            File.WriteAllText(scriptPath,
                "@echo off\r\n" +
                "chcp 65001>nul\r\n" +
                "title Games8Th.Team - PWA 筛查日志\r\n" +
                "echo.\r\n" +
                $"type \"{path}\"\r\n" +
                "echo.\r\n" +
                "echo [日志窗口保持打开] 请手动关闭此 CMD 窗口。\r\n",
                new UTF8Encoding(false));
            var startInfo = new ProcessStartInfo
            {
                FileName = Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe",
                UseShellExecute = true,
                CreateNoWindow = false,
                WindowStyle = ProcessWindowStyle.Normal
            };
            startInfo.ArgumentList.Add("/k");
            startInfo.ArgumentList.Add(scriptPath);
            Process.Start(startInfo);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[提示] 无法弹出日志 CMD 窗口：{ex.Message}");
        }
    }

    private static void WaitForManualClose()
    {
        using var exitSignal = new ManualResetEventSlim(false);
        ConsoleCancelEventHandler? cancelHandler = null;
        cancelHandler = (_, eventArgs) =>
        {
            eventArgs.Cancel = true;
            Console.WriteLine("检测到手动退出请求，正在关闭 PWA 屏蔽器。");
            exitSignal.Set();
        };
        Console.CancelKeyPress += cancelHandler;
        Console.WriteLine("当前动作已完成，PWA 屏蔽器将保持运行。");
        Console.WriteLine("请手动关闭此窗口，或按 Ctrl+C 退出。");
        try
        {
            exitSignal.Wait();
        }
        finally
        {
            Console.CancelKeyPress -= cancelHandler;
        }
    }

    private static int Usage(string? error)
    {
        if (!string.IsNullOrWhiteSpace(error)) Console.Error.WriteLine($"错误：{error}\n");
        Console.WriteLine("PWA屏蔽器.exe");
        Console.WriteLine("自动检测 PWA 安装目录的组件屏蔽工具\n");
        Console.WriteLine("用法：PWA屏蔽器.exe <status|scan|block|unblock|stop|watch> [选项]");
        Console.WriteLine("  --root <目录>       显式指定 PWA 安装目录（覆盖自动检测）");
        Console.WriteLine("  --all-exe           将目录内全部已核验 EXE 纳入阻断");
        Console.WriteLine("  --dry-run           只显示动作，不改系统状态");
        Console.WriteLine("  --what-if           --dry-run 的别名");
        Console.WriteLine("  --interval <秒>     watch 间隔，默认 3 秒");
        Console.WriteLine("\n默认 block 目标：主程序 + 已确认辅助程序 + MessageTransfer 驱动服务。");
        return string.IsNullOrWhiteSpace(error) ? 0 : 2;
    }

    private static int FinishAfterUsage(string? error)
    {
        var exitCode = Usage(error);
        if (ActiveTranscript is not null)
            ShowCompletionLog("help", ActiveRoot, exitCode, ActiveTranscript);
        WaitForManualClose();
        return exitCode;
    }

    private static string? DetectPwaRoot()
    {
        var candidates = new List<string>();
        var service = QueryService();
        var servicePath = service.Exists ? ExtractServiceBinaryPath(service.BinaryPath) : null;
        if (!string.IsNullOrWhiteSpace(servicePath))
        {
            var serviceDirectory = Path.GetDirectoryName(servicePath);
            var serviceRoot = serviceDirectory is null ? null : Path.GetDirectoryName(serviceDirectory);
            if (serviceRoot is not null) candidates.Add(serviceRoot);
        }

        candidates.AddRange(RegistryInstallRoots());
        candidates.AddRange(ProcessInstallRoots());
        candidates.AddRange(KnownInstallRoots());

        foreach (var candidate in candidates.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            string fullPath;
            try { fullPath = Path.GetFullPath(candidate); }
            catch { continue; }
            if (IsPwaRoot(fullPath)) return fullPath;
        }

        return null;
    }

    private static IEnumerable<string> RegistryInstallRoots()
    {
        var result = new List<string>();
        var paths = new[]
        {
            @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
            @"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
        };
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            foreach (var path in paths)
            {
                RegistryKey? uninstall = null;
                try
                {
                    uninstall = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view).OpenSubKey(path);
                    if (uninstall is null) continue;
                    foreach (var name in uninstall.GetSubKeyNames())
                    {
                        using var item = uninstall.OpenSubKey(name);
                        var displayName = item?.GetValue("DisplayName") as string;
                        var installLocation = item?.GetValue("InstallLocation") as string;
                        if ((!string.IsNullOrWhiteSpace(displayName) && displayName.Contains("完美世界", StringComparison.OrdinalIgnoreCase)) ||
                            (!string.IsNullOrWhiteSpace(installLocation) && installLocation.Contains("perfectworldarena", StringComparison.OrdinalIgnoreCase)))
                        {
                            if (!string.IsNullOrWhiteSpace(installLocation)) result.Add(installLocation);
                        }
                    }
                }
                catch { }
                finally { uninstall?.Dispose(); }
            }
        }
        return result;
    }

    private static IEnumerable<string> ProcessInstallRoots()
    {
        var result = new List<string>();
        foreach (var process in Process.GetProcesses())
        {
            try
            {
                var path = process.MainModule?.FileName;
                if (string.IsNullOrWhiteSpace(path)) continue;
                var directory = new DirectoryInfo(Path.GetDirectoryName(path)!);
                for (var current = directory; current is not null; current = current.Parent)
                {
                    if (current.Name.Equals("perfectworldarena", StringComparison.OrdinalIgnoreCase))
                    {
                        result.Add(current.FullName);
                        break;
                    }
                }
            }
            catch { }
            finally { process.Dispose(); }
        }
        return result;
    }

    private static IEnumerable<string> KnownInstallRoots()
    {
        var result = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            DefaultRoot,
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "perfectworldarena"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "perfectworldarena")
        };
        var drives = DriveInfo.GetDrives().Where(d => d.IsReady && d.DriveType == DriveType.Fixed);
        foreach (var drive in drives)
        {
            foreach (var parent in new[] { "Program Files", "Program Files (x86)", "Games", "Game", "perfectworldarena" })
            {
                result.Add(Path.Combine(drive.RootDirectory.FullName, parent, "perfectworldarena"));
                if (parent.Equals("perfectworldarena", StringComparison.OrdinalIgnoreCase))
                    result.Add(Path.Combine(drive.RootDirectory.FullName, parent));
            }
        }
        return result;
    }

    private static bool IsPwaRoot(string root)
    {
        try
        {
            if (!Directory.Exists(root)) return false;
            var evidence = ExpectedHashes.Keys.Count(key => File.Exists(Path.Combine(root, key)));
            var hasAnchor = File.Exists(Path.Combine(root, "完美世界竞技平台.exe")) ||
                            File.Exists(Path.Combine(root, @"plugin\MessageTransfer.sys"));
            return hasAnchor && evidence >= 2;
        }
        catch { return false; }
    }

    private static void WriteStatus(string status, string message)
    {
        var color = message.Contains("不适用", StringComparison.Ordinal) ? ConsoleColor.Yellow :
                    status is "匹配" or "已拦截" ? ConsoleColor.Green :
                    status is "未纳入规则" or "不匹配" or "缺失" ? ConsoleColor.Red : ConsoleColor.Gray;
        lock (ConsoleLock)
        {
            var previous = Console.ForegroundColor;
            Console.ForegroundColor = color;
            Console.WriteLine($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] [{status}] {message}");
            Console.ForegroundColor = previous;
        }
    }

    private static string RuleNameForPath(string path) =>
        $"{RulePrefix}-{Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes(path))).Substring(0, 12)}";

    private static bool IsPathBlocked(string path, IReadOnlyCollection<string> rules) =>
        rules.Contains(RuleNameForPath(path), StringComparer.OrdinalIgnoreCase);

    private static void PrintScreening(string root, bool allExe)
    {
        WriteStatus(Directory.Exists(root) ? "匹配" : "不匹配", $"筛查目标：{root}");
        if (!Directory.Exists(root))
        {
            WriteStatus("不匹配", "目标目录不存在 | 已拦截：否");
            return;
        }

        var rules = FirewallRules();
        var components = Inventory(root).ToDictionary(c => c.RelativePath, StringComparer.OrdinalIgnoreCase);
        var matched = 0;
        var mismatched = 0;
        var unruled = 0;
        foreach (var component in components.Values.OrderBy(c => c.RelativePath, StringComparer.OrdinalIgnoreCase))
        {
            var listed = ExpectedHashes.TryGetValue(component.RelativePath, out var expected);
            var isMatch = listed && component.Sha256.Equals(expected, StringComparison.OrdinalIgnoreCase);
            var status = !listed ? "未纳入规则" : isMatch ? "匹配" : "不匹配";
            if (status == "匹配") matched++;
            else if (status == "不匹配") mismatched++;
            else unruled++;
            var intercept = component.Extension.Equals(".exe", StringComparison.OrdinalIgnoreCase)
                ? IsPathBlocked(Path.Combine(root, component.RelativePath), rules) ? "是" : "否"
                : component.Extension.Equals(".sys", StringComparison.OrdinalIgnoreCase) ? "见服务状态" : "否（不适用）";
            WriteStatus(status, $"{component.RelativePath} | 当前哈希={component.Sha256} | 期望哈希={(listed ? expected : "未列入报告清单")} | 已拦截：{intercept}");
        }

        foreach (var key in ExpectedHashes.Keys.Where(key => !components.ContainsKey(key)).OrderBy(key => key, StringComparer.OrdinalIgnoreCase))
        {
            mismatched++;
            WriteStatus("缺失", $"{key} | 当前哈希=不存在 | 期望哈希={ExpectedHashes[key]} | 已拦截：否");
        }

        var service = QueryService();
        var serviceMatches = service.Exists && IsExpectedService(root, service);
        var serviceBlocked = serviceMatches && service.State.Equals("Stopped", StringComparison.OrdinalIgnoreCase) &&
                             service.StartType.Equals("Disabled", StringComparison.OrdinalIgnoreCase);
        WriteStatus(serviceMatches ? "匹配" : "不匹配",
            $"服务 {ServiceName} | 状态={ (service.Exists ? service.State : "未注册") } | 启动={ (service.Exists ? service.StartType : "未知") } | 已拦截：{(serviceBlocked ? "是" : "否")}");
        WriteStatus("普通", $"筛查汇总：匹配={matched}，不匹配/缺失={mismatched}，未纳入规则={unruled}，防火墙规则={rules.Count} 条");
    }

    private static int Status(string root, bool allExe)
    {
        WaitForPwaStart(root);
        PrintScreening(root, allExe);
        if (Directory.Exists(root))
        {
            WriteStatus("普通", $"PWA 进程：{FindPwaProcesses(root).Count} 个 | 服务状态快照：{(File.Exists(ServiceStatePath) ? "存在" : "无")}");
        }
        return 0;
    }

    private static void WaitForPwaStart(string root)
    {
        Console.WriteLine("等待完美世界平台启动...");
        while (true)
        {
            var processes = FindPwaProcesses(root);
            if (processes.Count > 0)
            {
                WriteStatus("匹配", $"已检测到完美世界平台进程：{string.Join("，", processes)}");
                break;
            }
            Thread.Sleep(1000); // 每秒检查一次
        }
    }
    private static int Scan(string root)
    {
        PrintScreening(root, false);
        if (!Directory.Exists(root)) return 1;
        return VerifyRequired(root) ? 0 : 1;
    }

    private static int Block(string root, bool allExe, bool dryRun)
    {
        EnsureAdministrator(dryRun);
        var verified = VerifyRequired(root);
        if (!verified) throw new InvalidOperationException("核心组件哈希校验失败，拒绝执行屏蔽。");
        var targets = ExecutableTargets(root, allExe);
        Console.WriteLine($"已核验核心组件，准备处理 {targets.Count} 个 EXE。");
        foreach (var path in targets) AddFirewallRule(path, dryRun);
        StopProcesses(root, allExe, dryRun);
        SetMessageTransfer(root, true, dryRun);
        return 0;
    }

    private static int Stop(string root, bool allExe, bool dryRun)
    {
        EnsureAdministrator(dryRun);
        if (!VerifyRequired(root)) throw new InvalidOperationException("核心组件哈希校验失败，拒绝停止进程/服务。");
        StopProcesses(root, allExe, dryRun);
        SetMessageTransfer(root, true, dryRun);
        return 0;
    }

    private static int Watch(string root, bool allExe, bool dryRun, int interval)
    {
        EnsureAdministrator(dryRun);
        WaitForPwaStart(root);
        using var stopSignal = new ManualResetEventSlim(false);
        ConsoleCancelEventHandler cancelHandler = (_, eventArgs) =>
        {
            eventArgs.Cancel = true;
            WriteStatus("普通", "收到 Ctrl+C，正在结束持续筛查。");
            stopSignal.Set();
        };
        Console.CancelKeyPress += cancelHandler;
        try
        {
            WriteStatus("普通", $"开始实时循环拦截：{root} | 间隔={interval}s | 持续运行中，关闭日志窗口即结束");
            while (!stopSignal.IsSet)
            {
                PrintScreening(root, allExe);
                if (VerifyRequired(root))
                {
                    StopProcesses(root, allExe, dryRun);
                    SetMessageTransfer(root, true, dryRun);
                }
                else
                {
                    WriteStatus("不匹配", "本轮校验失败，跳过停止进程和服务 | 已拦截：否");
                }
                stopSignal.Wait(TimeSpan.FromSeconds(interval));
            }
        }
        finally
        {
            Console.CancelKeyPress -= cancelHandler;
        }
        return 0;
    }

    private static int Unblock(string root, bool dryRun)
    {
        EnsureAdministrator(dryRun);
        foreach (var rule in FirewallRules())
        {
            Console.WriteLine($"{(dryRun ? "[预演] 将删除" : "删除")}防火墙规则：{rule}");
            if (!dryRun) Run("netsh", $"advfirewall firewall delete rule name=\"{rule}\"");
        }
        RestoreMessageTransfer(root, dryRun);
        return 0;
    }

    private static List<Component> Inventory(string root)
    {
        var extensions = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ".exe", ".dll", ".sys", ".node" };
        return Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
            .Where(path => extensions.Contains(Path.GetExtension(path)))
            .Select(path => new Component(Relative(root, path), Path.GetExtension(path), new FileInfo(path).Length, ComputeHash(path)))
            .OrderBy(c => c.RelativePath, StringComparer.OrdinalIgnoreCase).ToList();
    }

    private static bool VerifyRequired(string root)
    {
        foreach (var key in ExpectedHashes.Keys)
        {
            var path = Path.Combine(root, key);
            if (!File.Exists(path)) { WriteStatus("缺失", $"核心组件缺失：{key} | 已拦截：否"); return false; }
            var actual = ComputeHash(path);
            if (!actual.Equals(ExpectedHashes[key], StringComparison.OrdinalIgnoreCase))
            {
                WriteStatus("不匹配", $"核心组件哈希不匹配：{key} | 当前哈希={actual} | 期望哈希={ExpectedHashes[key]} | 已拦截：否");
                return false;
            }
        }
        return true;
    }

    private static List<string> ExecutableTargets(string root, bool allExe)
    {
        var candidates = allExe
            ? Directory.EnumerateFiles(root, "*.exe", SearchOption.AllDirectories).ToList()
            : BlockedExecutableNames.Select(name => Path.Combine(root, name)).ToList();
        return candidates.Where(File.Exists).Where(path =>
        {
            var relative = Relative(root, path);
            return ExpectedHashes.TryGetValue(relative, out var expected) &&
                   ComputeHash(path).Equals(expected, StringComparison.OrdinalIgnoreCase);
        }).OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToList();
    }

    private static void AddFirewallRule(string path, bool dryRun)
    {
        var name = RuleNameForPath(path);
        if (FirewallRules().Contains(name, StringComparer.OrdinalIgnoreCase)) { Console.WriteLine($"规则已存在：{name}"); return; }
        Console.WriteLine($"{(dryRun ? "[预演] 将创建" : "创建")}出站阻断规则：{name}\n  程序：{path}");
        if (!dryRun) Run("netsh", $"advfirewall firewall add rule name=\"{name}\" dir=out action=block program=\"{path}\" profile=any enable=yes");
    }

    private static void StopProcesses(string root, bool allExe, bool dryRun)
    {
        var allowed = new HashSet<string>(ExecutableTargets(root, allExe), StringComparer.OrdinalIgnoreCase);
        foreach (var process in Process.GetProcesses())
        {
            try
            {
                var path = process.MainModule?.FileName;
                if (path is null || !allowed.Contains(path)) continue;
                Console.WriteLine($"{(dryRun ? "[预演] 将停止" : "停止")} PID {process.Id}：{path}");
                if (!dryRun) { process.Kill(true); process.WaitForExit(3000); }
            }
            catch (Exception ex) { Console.WriteLine($"[跳过] PID {process.Id}：{ex.Message}"); }
            finally { process.Dispose(); }
        }
    }

    private static List<string> FindPwaProcesses(string root)
    {
        var result = new List<string>();
        foreach (var process in Process.GetProcesses())
        {
            try { if (process.MainModule?.FileName?.StartsWith(root, StringComparison.OrdinalIgnoreCase) == true) result.Add($"{process.Id}:{process.MainModule.FileName}"); }
            catch { }
            finally { process.Dispose(); }
        }
        return result;
    }

    private static ServiceSnapshot QueryService()
    {
        try
        {
            var text = Run("sc", $"query {ServiceName}", false);
            var qc = Run("sc", $"qc {ServiceName}", false);
            if (!text.Contains("SERVICE_NAME:", StringComparison.OrdinalIgnoreCase) ||
                !qc.Contains("SERVICE_NAME:", StringComparison.OrdinalIgnoreCase))
                return new ServiceSnapshot(false, "", "", "");
            return new ServiceSnapshot(true,
                text.Contains("RUNNING", StringComparison.OrdinalIgnoreCase) ? "Running" : text.Contains("STOPPED", StringComparison.OrdinalIgnoreCase) ? "Stopped" : "Unknown",
                qc.Contains("BOOT_START", StringComparison.OrdinalIgnoreCase) ? "Boot" :
                qc.Contains("SYSTEM_START", StringComparison.OrdinalIgnoreCase) ? "System" :
                qc.Contains("AUTO_START", StringComparison.OrdinalIgnoreCase) ? "Auto" :
                qc.Contains("DEMAND_START", StringComparison.OrdinalIgnoreCase) ? "Manual" :
                qc.Contains("DISABLED", StringComparison.OrdinalIgnoreCase) ? "Disabled" : "Unknown",
                ExtractLine(qc, "BINARY_PATH_NAME"));
        }
        catch { return new ServiceSnapshot(false, "", "", ""); }
    }

    private static void SetMessageTransfer(string root, bool block, bool dryRun)
    {
        var service = QueryService();
        if (!service.Exists) { Console.WriteLine($"服务 {ServiceName} 未注册，跳过服务操作。"); return; }
        if (!IsExpectedService(root, service))
        {
            Console.WriteLine($"[拒绝] {ServiceName} 的路径不是当前 PWA 目录中的 plugin\\MessageTransfer.sys，跳过服务操作。");
            return;
        }
        if (service.State is not ("Running" or "Stopped") || ScStartValue(service.StartType) is null)
        {
            Console.WriteLine($"[拒绝] 无法可靠识别 {ServiceName} 的当前状态或启动类型，跳过服务操作。");
            return;
        }

        if (!block) return;
        if (!dryRun && !File.Exists(ServiceStatePath)) SaveServiceState(service);
        else if (!dryRun) Console.WriteLine($"沿用已有服务状态快照：{ServiceStatePath}");

        if (string.Equals(service.State, "Running", StringComparison.OrdinalIgnoreCase))
            RunServiceCommand("stop MessageTransfer", dryRun);
        if (!string.Equals(service.StartType, "Disabled", StringComparison.OrdinalIgnoreCase))
            RunServiceCommand("config MessageTransfer start= disabled", dryRun);
    }

    private static void RestoreMessageTransfer(string root, bool dryRun)
    {
        var saved = LoadServiceState();
        if (saved is null)
        {
            Console.WriteLine("未找到本工具保存的 MessageTransfer 服务快照，不修改服务配置。");
            return;
        }

        var service = QueryService();
        if (!service.Exists)
        {
            Console.WriteLine($"[保留快照] 服务 {ServiceName} 当前未注册，无法恢复。");
            return;
        }
        if (!IsExpectedService(root, service) || !string.Equals(saved.BinaryPath, service.BinaryPath, StringComparison.OrdinalIgnoreCase))
        {
            Console.WriteLine("[保留快照] 服务路径已变化，拒绝将快照应用到不同的驱动。");
            return;
        }
        if (service.State is not ("Running" or "Stopped") || ScStartValue(service.StartType) is null)
        {
            Console.WriteLine($"[保留快照] 无法可靠识别 {ServiceName} 的当前状态或启动类型，暂不删除快照。");
            return;
        }

        var startValue = ScStartValue(saved.StartType);
        if (startValue is null || saved.State is not ("Running" or "Stopped"))
        {
            Console.WriteLine("[保留快照] 快照内容不完整，无法安全恢复。");
            return;
        }

        if (saved.State == "Stopped" && service.State == "Running")
            RunServiceCommand("stop MessageTransfer", dryRun);
        if (!string.Equals(saved.StartType, service.StartType, StringComparison.OrdinalIgnoreCase))
            RunServiceCommand($"config MessageTransfer start= {startValue}", dryRun);
        if (saved.State == "Running" && service.State != "Running")
            RunServiceCommand("start MessageTransfer", dryRun);

        if (!dryRun)
        {
            File.Delete(ServiceStatePath);
            Console.WriteLine("已按快照恢复 MessageTransfer，快照已删除。");
        }
        else Console.WriteLine($"[预演] 恢复完成后将删除服务状态快照：{ServiceStatePath}");
    }

    private static void RunServiceCommand(string command, bool dryRun)
    {
        Console.WriteLine($"{(dryRun ? "[预演] 将" : "")}执行服务操作：sc {command}");
        if (!dryRun) Run("sc", command);
    }

    private static void SaveServiceState(ServiceSnapshot service)
    {
        var directory = Path.GetDirectoryName(ServiceStatePath) ?? throw new InvalidOperationException("无法确定服务状态目录。");
        Directory.CreateDirectory(directory);
        var json = JsonSerializer.Serialize(new SavedServiceState(ServiceName, service.State, service.StartType, service.BinaryPath), new JsonSerializerOptions { WriteIndented = true });
        var temporaryPath = ServiceStatePath + ".tmp";
        File.WriteAllText(temporaryPath, json, Encoding.UTF8);
        File.Move(temporaryPath, ServiceStatePath, true);
        Console.WriteLine($"已保存服务状态快照：{ServiceStatePath}");
    }

    private static SavedServiceState? LoadServiceState()
    {
        try
        {
            if (!File.Exists(ServiceStatePath)) return null;
            var saved = JsonSerializer.Deserialize<SavedServiceState>(File.ReadAllText(ServiceStatePath));
            return saved is not null && string.Equals(saved.ServiceName, ServiceName, StringComparison.OrdinalIgnoreCase) ? saved : null;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[保留快照] 读取服务状态快照失败：{ex.Message}");
            return null;
        }
    }

    private static bool IsExpectedService(string root, ServiceSnapshot service)
    {
        var expected = Path.GetFullPath(Path.Combine(root, @"plugin\MessageTransfer.sys"));
        var actual = ExtractServiceBinaryPath(service.BinaryPath);
        return actual is not null && string.Equals(Path.GetFullPath(actual), expected, StringComparison.OrdinalIgnoreCase);
    }

    private static string? ExtractServiceBinaryPath(string value)
    {
        var path = value.Trim();
        if (path.StartsWith("\\??\\", StringComparison.Ordinal)) path = path[4..];
        if (path.StartsWith('"'))
        {
            var endQuote = path.IndexOf('"', 1);
            return endQuote > 1 ? path[1..endQuote] : null;
        }
        var sysEnd = path.IndexOf(".sys", StringComparison.OrdinalIgnoreCase);
        return sysEnd >= 0 ? path[..(sysEnd + 4)] : path;
    }

    private static string? ScStartValue(string startType) => startType switch
    {
        "Boot" => "boot",
        "System" => "system",
        "Auto" => "auto",
        "Manual" => "demand",
        "Disabled" => "disabled",
        _ => null
    };

    private static List<string> FirewallRules() =>
        Run("netsh", "advfirewall firewall show rule name=all", false)
            .Split('\n', StringSplitOptions.RemoveEmptyEntries)
            .Select(line => line.Trim())
            .Where(line => line.StartsWith("Rule Name:", StringComparison.OrdinalIgnoreCase))
            .Select(line => line[(line.IndexOf(':') + 1)..].Trim())
            .Where(line => line.StartsWith(RulePrefix, StringComparison.OrdinalIgnoreCase)).Distinct(StringComparer.OrdinalIgnoreCase).ToList();

    private static string ComputeHash(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static string Relative(string root, string path) => Path.GetRelativePath(root, path).Replace('/', '\\');

    private static string ExtractLine(string text, string prefix)
    {
        var line = text.Split('\n').Select(x => x.Trim()).FirstOrDefault(x => x.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
        if (line is null) return "";
        var colon = line.IndexOf(':');
        return colon >= 0 ? line[(colon + 1)..].Trim() : line;
    }

    private static void EnsureAdministrator(bool dryRun)
    {
        if (dryRun) return;
        using var identity = WindowsIdentity.GetCurrent();
        if (!(new WindowsPrincipal(identity)).IsInRole(WindowsBuiltInRole.Administrator)) throw new InvalidOperationException("此操作需要管理员权限。");
    }

    private static string Run(string file, string arguments, bool throwOnError = true)
    {
        using var process = Process.Start(new ProcessStartInfo(file, arguments) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true }) ?? throw new InvalidOperationException($"无法启动 {file}");
        var stdout = process.StandardOutput.ReadToEnd();
        var stderr = process.StandardError.ReadToEnd();
        process.WaitForExit();
        if (throwOnError && process.ExitCode != 0) throw new InvalidOperationException($"{file} 失败：{stderr.Trim()}");
        return stdout + stderr;
    }
}


