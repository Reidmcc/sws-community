// Readable names for value keys and configs.
//   Fields: "weaponfiring.laserregenpips[3].costpershot" -> "Weapon Firing › Laser Regen Pips [3] › Cost Per Shot", from
//   the game's own CamelCase field names (Words.cs, generated).
//   Configs: from the name the game gives the asset (defaults.ini "; name=" lines, written by the DLL), split at its own
//   capitals: "HullCustom_Chameleon_Xwing" -> "Hull Custom Chameleon X-wing".
// Where the game's spelling isn't available (an older defaults.ini; the attributes a component modifies), a lowercase
// name is split into words of the game's vocabulary, leaving anything unknown whole rather than chopping it up.
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
        // words written in capitals in labels (only real acronyms: the game's vocabulary has many short scraps)
        static readonly HashSet<string> Acronym = new HashSet<string>(StringComparer.Ordinal) { "ai", "ui", "hud", "lod", "fov", "id", "fx", "vfx", "sfx", "aoe", "emp", "ls" };
        static readonly Regex Camel = new Regex(@"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|\d+");

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
                if (p.EndsWith(" Attribute")) p = p.Substring(0, p.Length - 10);   // the game's marker for attribute-backed fields
                parts.Add(idx.Length > 0 ? p + " " + idx : p);
            }
            // a vector's component reads as a suffix: "Afterburner Axis Turn Rate (Y)"
            if (parts.Count > 1 && (parts[parts.Count - 1] == "x" || parts[parts.Count - 1] == "y" || parts[parts.Count - 1] == "z"))
            {
                parts[parts.Count - 2] += " (" + parts[parts.Count - 1].ToUpperInvariant() + ")";
                parts.RemoveAt(parts.Count - 1);
            }
            r = string.Join(" › ", parts);
            cache[key] = r;
            return r;
        }

        // the game's abbreviations in config names, spelled out ("ls" is LiveService, a content drop, and says nothing)
        static readonly Dictionary<string, string> Abbrev = new Dictionary<string, string>(StringComparer.Ordinal) {
            { "las", "Laser" }, { "reb", "Rebel" }, { "imp", "Imperial" }, { "ls", "" }, { "pvb", "PvB" }, { "sp", "SP" },
            { "ai", "AI" }, { "bot", "Bot" } };
        static readonly string[] TypePrefixes = Limits.Types;

        // a config's readable name. Full: "Engine Custom Agile Y-wing". Short, for the tree (which already shows the ship
        // and the kind of config): the distinctive part, "Agile"; the full name where nothing else is left.
        // original: the asset's name as the game spells it, when defaults.ini has it.
        public static string Config(string asset, bool shortForm, string original = null)
        {
            string[] parts = (original != null && original.ToLowerInvariant() == asset ? original : asset)
                .Split(new[] { '_' }, StringSplitOptions.RemoveEmptyEntries);
            var full = new List<string>(); var rest = new List<string>();
            for (int i = 0; i < parts.Length; i++)
            {
                string part = parts[i], low = part.ToLowerInvariant(), word;
                string ship = Catalog.Ships.FirstOrDefault(s => low == s || low == s + "ai");
                if (ship != null)
                {
                    full.Add(Catalog.ShipName(ship) + (low != ship ? " AI" : ""));
                    if (low != ship) rest.Add("AI");
                    continue;
                }
                string cap = Catalog.CapitalName(low);
                if (cap != null) { full.Add(cap); continue; }
                if (Abbrev.TryGetValue(low, out word)) { if (word.Length > 0) { full.Add(word); rest.Add(word); } continue; }
                if (i == 0)
                {
                    string t = TypePrefixes.FirstOrDefault(x => low.StartsWith(x, StringComparison.Ordinal)) ?? low;
                    full.Add(Phrase(part.Substring(0, t.Length)));
                    string tail = part.Substring(t.Length);
                    if (tail.Length > 0 && !Abbrev.ContainsKey(tail.ToLowerInvariant())) { word = Phrase(tail); full.Add(word); rest.Add(word); }
                    continue;
                }
                word = Phrase(part);
                full.Add(word); rest.Add(word);
            }
            string f = string.Join(" ", full.Where(w => w.Length > 0));
            return shortForm && rest.Count > 0 ? string.Join(" ", rest) : f;
        }

        // one piece of a name: split at its own capitals if it has them ("HighYieldTorpedo"), else by vocabulary
        static string Phrase(string s)
        {
            if (s.Skip(1).Any(char.IsUpper) || (s.Length > 0 && char.IsUpper(s[0]) && s.Skip(1).All(char.IsLower)))
                return string.Join(" ", Camel.Matches(s).Cast<Match>().Select(m => Title(m.Value.ToLowerInvariant())));
            return Name(s.ToLowerInvariant());
        }

        // a lowercase name: exact if the game has it, otherwise split at underscores and into words
        public static string Name(string name)
        {
            Init();
            string e;
            if (exact.TryGetValue(name, out e)) return e;
            return string.Join(" ", name.Split('_').Where(s => s.Length > 0).Select(Split));
        }

        static string Title(string w)
        {
            if (Acronym.Contains(w)) return w.ToUpperInvariant();
            return w.Length == 0 ? w : char.ToUpperInvariant(w[0]) + w.Substring(1);
        }

        // the cheapest split into words: a known word of 3+ letters costs 2, of 2 letters 5, of 1 letter 8; an unknown
        // stretch of 3+ letters costs 7 as one word, so "torpedo" stays whole rather than "t or pe do", while three known
        // words ("starship movement config", 6) beat one unknown stretch
        static string Split(string s)
        {
            if (exact.ContainsKey(s)) return exact[s];
            int n = s.Length;
            var cost = new int[n + 1]; var from = new int[n + 1];
            for (int i = 1; i <= n; i++) { cost[i] = int.MaxValue; from[i] = i - 1; }
            for (int i = 0; i < n; i++)
            {
                if (cost[i] == int.MaxValue) continue;
                for (int len = 1; len <= n - i; len++)
                {
                    string w = s.Substring(i, len);
                    int c;
                    if (w.All(char.IsDigit)) c = 1;
                    else if (len <= maxWord && vocab.Contains(w)) c = len >= 3 ? 2 : len == 2 ? 5 : 8;
                    else c = len >= 3 ? 7 : 20;
                    if (cost[i] + c < cost[i + len]) { cost[i + len] = cost[i] + c; from[i + len] = i; }
                }
            }
            var words = new List<string>();
            for (int j = n; j > 0; j = from[j]) words.Insert(0, s.Substring(from[j], j - from[j]));
            return string.Join(" ", words.Select(Title));
        }
    }
}
