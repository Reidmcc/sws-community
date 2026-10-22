// Prints what the settings app computes for profiles: the effective values, notes and fingerprint, for comparing with
// the fingerprint the DLL logs ("profile: '<name>': ... fingerprint xxxxxxxx"). Built from Core.cs alone.
// Usage: sws_settings_test.exe <folder with server.ini, profiles\, limits.ini> <profile> [<profile> ...]
using System;
using System.Linq;

namespace SwsSettings
{
    static class SelfTest
    {
        static int Main(string[] args)
        {
            if (args.Length < 2) { Console.WriteLine("usage: sws_settings_test <dir> <profile> [<profile> ...] | <dir> --words"); return 2; }
            if (args[1] == "--words")   // every field in defaults.ini with its readable name, for review
            {
                var d = Defaults.Load(args[0]);
                foreach (var f in d.Assets.SelectMany(a => a.Value.Keys).Distinct().OrderBy(x => x, StringComparer.Ordinal))
                    Console.WriteLine(f + "\t" + Words.Pretty(f));
                return 0;
            }
            var limits = Limits.Load(args[0]);
            Console.WriteLine("limits: " + (limits.Loaded ? limits.ByKey.Count + " fields, " + limits.Primary.Count + " primary weapon configs" : "none"));
            for (int i = 1; i < args.Length; i++)
            {
                var p = Profile.Load(args[0], args[i]);
                var e = Effective.Of(p, limits);
                Console.WriteLine(args[i] + ": rules pinball=" + (p.NoPinball ? 1 : 0) + " multidrift=" + (p.NoMultidrift ? 1 : 0) + ", " +
                                  p.Values.Count + " value(s), " + e.Values.Count + " applied, fingerprint " + Effective.Show(e.Fingerprint));
                foreach (var n in e.Notes) Console.WriteLine("   " + n);
            }
            return 0;
        }
    }
}
