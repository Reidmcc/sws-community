// What a value does, for the hover over its row, and its display name where the generated one reads badly. The data is
// Tips.cs (tools/gen_app_tips.py, from tools/value_tips.tsv); this is the lookup.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace SwsSettings
{
    static partial class Tips
    {
        class Entry { public string Name, Tip, Evidence; }
        static Dictionary<string, Entry> byKey;

        static void Init()
        {
            if (byKey != null) return;
            byKey = Rows.ToDictionary(r => r[0], r => new Entry { Name = r[1], Tip = r[2], Evidence = r[3] }, StringComparer.Ordinal);
        }

        // the tip's key for a config's field: its type (the pools of limits.ini folded together) and the field without [n]
        public static string KeyOf(string asset, string field)
        {
            string type = Limits.Types.FirstOrDefault(t => asset.StartsWith(t, StringComparison.Ordinal));
            if (type == null) return null;
            if (type == "enginecustom" || type == "hullcustom" || type == "shieldcustom") type = "components";
            return type + "." + Regex.Replace(field, @"\[[^\]]*\]", "");
        }

        // the field's display name: the generated one, with its last part replaced where a better name is known
        public static string Display(string asset, string field)
        {
            Init();
            string pretty = Words.Pretty(field);
            Entry e; string key = KeyOf(asset, field);
            if (key == null || !byKey.TryGetValue(key, out e) || e.Name.Length == 0) return pretty;
            // the last part keeps its index ("[2]"): an array's rows differ only by it
            string idx = Regex.Match(field.Split('.').Last(), @"(\[\d+\])+$").Value;
            string name = idx.Length > 0 ? e.Name + " " + idx : e.Name;
            int cut = pretty.LastIndexOf(" › ", StringComparison.Ordinal);
            return cut < 0 ? name : pretty.Substring(0, cut + 3) + name;
        }

        // the hover text: what the value does, wrapped, with a note when it comes from the name alone; null if none is known
        public static string Text(string asset, string field)
        {
            Init();
            Entry e; string key = KeyOf(asset, field);
            if (key == null || !byKey.TryGetValue(key, out e)) return null;
            string t = Wrap(e.Tip, 72);
            if (e.Evidence == "name") t += "\n" + Wrap("(From the name and default values; not checked in the game's code.)", 72);
            return t;
        }

        public static IEnumerable<string[]> All() { Init(); return Rows; }

        static string Wrap(string s, int width)
        {
            var sb = new StringBuilder(); int col = 0;
            foreach (var w in s.Split(' '))
            {
                if (col > 0 && col + 1 + w.Length > width) { sb.Append('\n'); col = 0; }
                else if (col > 0) { sb.Append(' '); col++; }
                sb.Append(w); col += w.Length;
            }
            return sb.ToString();
        }
    }
}
