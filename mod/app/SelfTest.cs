// Prints what the settings app computes for profiles: the effective values, notes and fingerprint, for comparing with
// the fingerprint the DLL logs ("profile: '<name>': ... fingerprint xxxxxxxx"). Built from Core.cs alone.
// Usage: sws_settings_test.exe <folder with server.ini, profiles\, limits.ini> <profile> [<profile> ...]
using System;
using System.Collections.Generic;
using System.Linq;

namespace SwsSettings
{
    static class SelfTest
    {
        static int Main(string[] args)
        {
            if (args.Length < 2) { Console.WriteLine("usage: sws_settings_test <dir> <profile> [<profile> ...] | <dir> --words | <dir> --configs | <dir> --tips"); return 2; }
            if (args[1] == "--words")   // every field in defaults.ini with its readable name, for review
            {
                var d = Defaults.Load(args[0]);
                foreach (var f in d.Assets.SelectMany(a => a.Value.Keys).Distinct().OrderBy(x => x, StringComparer.Ordinal))
                    Console.WriteLine(f + "\t" + Words.Pretty(f));
                return 0;
            }
            if (args[1] == "--tips")   // every field the app shows (one with a limit), its tip key, display name and tip; exit 1 if any lacks a tip
            {
                var d = Defaults.Load(args[0]); var lim = Limits.Load(args[0]);
                var shown = new SortedDictionary<string, string>(StringComparer.Ordinal);
                foreach (var a in d.Assets)
                    foreach (var f in a.Value.Keys)
                        if (lim.Find(a.Key, f) != null) { string k = Tips.KeyOf(a.Key, f); if (k != null && !shown.ContainsKey(k)) shown[k] = a.Key + "\t" + f; }
                int missing = 0;
                foreach (var kv in shown)
                {
                    string[] af = kv.Value.Split('\t'); string tip = Tips.Text(af[0], af[1]);
                    if (tip == null) missing++;
                    Console.WriteLine(kv.Key + "\t" + Tips.Display(af[0], af[1]) + "\t" + (tip == null ? "(NO TIP)" : tip.Replace("\n", " ")));
                }
                Console.WriteLine(shown.Count + " values shown, " + missing + " without a tip");
                return missing == 0 ? 0 : 1;
            }
            if (args[1] == "--configs")   // every config's short and full readable name, for review
            {
                var d = Defaults.Load(args[0]);
                foreach (var a in d.Assets.Keys)
                    Console.WriteLine(a + "\t" + Words.Config(a, true, d.NameOf(a)) + "\t" + Words.Config(a, false, d.NameOf(a)) + "\t" + (d.NameOf(a) ?? "(no name)"));
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
