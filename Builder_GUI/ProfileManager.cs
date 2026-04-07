using System;
using System.IO;
using System.Text.Json;

namespace ShatteredMirror_Builder
{
    public class TargetProfile
    {
        public string ProfileName { get; set; } = string.Empty;
        public string C2Domain { get; set; } = string.Empty;
        public int C2Port { get; set; }
        public string PSKSeed { get; set; } = string.Empty;
        
        // Atom Boolean Flags
        public bool EnableNet { get; set; }
        public bool EnableSys { get; set; }
        public bool EnableKeylogger { get; set; }
        public bool EnableExfil { get; set; }
        public bool EnableScreen { get; set; }
        public bool EnablePersist { get; set; }
        public bool EnableProcInj { get; set; }
        public bool EnableFileSys { get; set; }
        
        public string DecoyPath { get; set; } = string.Empty;
    }

    public class ProfileManager
    {
        /*
         * Saves and loads target configuration profiles to disk (.json)
         */

        private static readonly string PROFILE_DIR = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Profiles");

        public static void SaveProfile(TargetProfile profile)
        {
            if (!Directory.Exists(PROFILE_DIR))
            {
                Directory.CreateDirectory(PROFILE_DIR);
            }

            string safeName = string.Join("_", profile.ProfileName.Split(Path.GetInvalidFileNameChars()));
            string path = Path.Combine(PROFILE_DIR, $"{safeName}.json");

            var options = new JsonSerializerOptions { WriteIndented = true };
            string jsonString = JsonSerializer.Serialize(profile, options);
            
            File.WriteAllText(path, jsonString);
        }

        public static TargetProfile? LoadProfile(string profileName)
        {
            string safeName = string.Join("_", profileName.Split(Path.GetInvalidFileNameChars()));
            string path = Path.Combine(PROFILE_DIR, $"{safeName}.json");

            if (!File.Exists(path)) return null;

            string jsonString = File.ReadAllText(path);
            return JsonSerializer.Deserialize<TargetProfile>(jsonString);
        }
    }

}
