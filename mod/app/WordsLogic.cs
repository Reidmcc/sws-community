// Readable names for value keys: "weaponfiring.laserregenpips[3].costpershot" -> "weapon firing › laser regen pips [3] ›
// cost per shot". Exact names come from the game's own CamelCase field names (Words.cs, generated); anything else, like
// the attributes a component modifies, is split into the fewest words of the game's vocabulary.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

namespace SwsSettings
{
    static partial class Words
    {
        static Dictionary<string, string> exact;
        static HashSet<string> vocab;
        static int maxWord;
        static readonly Dictionary<string, string> cache = new Dictionary<string, string>(StringComparer.Ordinal);

        static void Init()
        {
            if (exact != null) return;
            exact = ExactPairs.ToDictionary(p => p[0], p => p[1], StringComparer.Ordinal);
            vocab = new HashSet<string>(Vocabulary, StringComparer.Ordinal);
            maxWord = Vocabulary.Max(w => w.Length);
        }

        // a whole field path, part by part
        public static string Pretty(string key)
        {
            string r;
            if (cache.TryGetValue(key, out r)) return r;
            var parts = new List<string>();
            foreach (var part in key.Split('.'))
            {
                var m = Regex.Match(part, @"^(.*?)((\[\d+\])*)$");
                string name = m.Groups[1].Value, idx = m.Groups[2].Value;
                string p = name.Length == 1 ? name : Name(name);
                if (p.EndsWith(" attribute")) p = p.Substring(0, p.Length - 10);   // the game's marker for attribute-backed fields
                parts.Add(idx.Length > 0 ? p + " " + idx : p);
            }
            // a vector's component reads as a suffix: "afterburner axis turn rate (y)"
            if (parts.Count > 1 && (parts[parts.Count - 1] == "x" || parts[parts.Count - 1] == "y" || parts[parts.Count - 1] == "z"))
            {
                parts[parts.Count - 2] += " (" + parts[parts.Count - 1] + ")";
                parts.RemoveAt(parts.Count - 1);
            }
            r = string.Join(" › ", parts);
            cache[key] = r;
            return r;
        }

        // one name: exact if the game has it, otherwise split at underscores and into the fewest known words
        public static string Name(string name)
        {
            Init();
            string e;
            if (exact.TryGetValue(name, out e)) return e;
            return string.Join(" ", name.Split('_').Where(s => s.Length > 0).Select(Split));
        }

        static string Split(string s)
        {
            if (exact.ContainsKey(s)) return exact[s];
            int n = s.Length;
            var cost = new int[n + 1]; var from = new int[n + 1];
            for (int i = 1; i <= n; i++) { cost[i] = int.MaxValue; from[i] = i - 1; }
            for (int i = 0; i < n; i++)
            {
                if (cost[i] == int.MaxValue) continue;
                // a known word costs 1; a run of digits costs 1; an unknown letter costs 10 (so known words always win)
                for (int len = 1; len <= Math.Min(maxWord, n - i); len++)
                {
                    string w = s.Substring(i, len);
                    int c = vocab.Contains(w) || w.All(char.IsDigit) ? 1 : (len == 1 ? 10 : int.MaxValue);
                    if (c == int.MaxValue) continue;
                    if (cost[i] + c < cost[i + len]) { cost[i + len] = cost[i] + c; from[i + len] = i; }
                }
            }
            var words = new List<string>();
            for (int j = n; j > 0; j = from[j]) words.Insert(0, s.Substring(from[j], j - from[j]));
            // stray single letters from unknown stretches are glued back together
            var merged = new List<string>();
            foreach (var w in words)
                if (merged.Count > 0 && w.Length == 1 && !vocab.Contains(w) && merged[merged.Count - 1].Length >= 1 && !vocab.Contains(merged[merged.Count - 1]))
                    merged[merged.Count - 1] += w;
                else merged.Add(w);
            return string.Join(" ", merged);
        }
    }
}
