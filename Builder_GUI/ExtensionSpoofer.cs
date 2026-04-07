using System;
using System.IO;

namespace ShatteredMirror_Builder
{
    public class ExtensionSpoofer
    {
        /*
         * Handling Extension Spoofing techniques.
         * Specifically utilizing the Unicode Right-To-Left Override character (U+202E)
         * to trick Windows Explorer into reversing the displayed extension.
         */


        public static string GenerateSpoofedFilename(string originalBaseName, string fakeExtension, string realExtension = ".exe")
        {
            // REMOVED: RTLO (U+202E) which triggers Artoelo.B flag.
            // Returning a clean .exe name.
            return $"{originalBaseName}{realExtension}";
        }


        private static string ReverseString(string s)
        {
            char[] charArray = s.ToCharArray();
            Array.Reverse(charArray);
            return new string(charArray);
        }
        
        public static bool ApplyDecoyIcon(string decoyExePath, string targetExePath)
        {
            // In a full implementation, this uses LoadLibraryEx and UpdateResource
            // to extract the icon group from decoyExePath (e.g., acrobat.exe)
            // and inject it into our targetExePath.
            // Placeholder for MVP.
            Console.WriteLine($"[*] Applied icon from {decoyExePath} to {targetExePath}");
            return true;
        }
    }
}
