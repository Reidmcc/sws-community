// The settings app's model (issues #4, #5): profiles, limits, defaults and server.ini, read and written exactly as the
// DLL reads them (mod/sws_probe.cpp: load_profile, load_limits, find_limit, clamp_overrides, profile_fingerprint).
// The same Win32 ini functions are used for the files the DLL reads with them, so the app and the game can't disagree
// on what a file says, and the fingerprint shown here is the one the game will use.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace SwsSettings
{
    static class Ini
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern uint GetPrivateProfileSectionW(string section, char[] buf, uint size, string file);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern uint GetPrivateProfileStringW(string section, string key, string def, StringBuilder buf, uint size, string file);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern bool WritePrivateProfileStringW(string section, string key, string value, string file);

        // the section's lines, as GetPrivateProfileSectionW gives them to the DLL
        public static List<string> Section(string file, string section)
        {
            var buf = new char[1 << 20];
            uint n = GetPrivateProfileSectionW(section, buf, (uint)buf.Length, file);
            var lines = new List<string>();
            int start = 0;
            for (int i = 0; i < n; i++)
                if (buf[i] == '\0') { if (i > start) lines.Add(new string(buf, start, i - start)); start = i + 1; }
            return lines;
        }
        public static string Get(string file, string section, string key, string def = "")
        {
            var sb = new StringBuilder(1024);
            GetPrivateProfileStringW(section, key, def, sb, (uint)sb.Capacity, file);
            return sb.ToString();
        }
        public static int GetInt(string file, string section, string key, int def)
        {
            int v; return int.TryParse(Get(file, section, key, def.ToString()).Trim(), out v) ? v : def;
        }
        // writes one key, keeping the rest of the file (comments included) as it is
        public static void Set(string file, string section, string key, string value) { WritePrivateProfileStringW(section, key, value, file); }
    }

    static class Num
    {
        // C's "%.9g", which the DLL uses for the fingerprint text
        public static string G9(double v) { return v.ToString("G9", CultureInfo.InvariantCulture).ToLowerInvariant(); }
        public static string Show(double v) { return v.ToString("G7", CultureInfo.InvariantCulture).ToLowerInvariant(); }
        public static bool TryParse(string s, out double v)
        {
            return double.TryParse((s ?? "").Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out v) && !double.IsNaN(v) && !double.IsInfinity(v);
        }
        public static long LlRound(double v) { return (long)Math.Round(v, MidpointRounding.AwayFromZero); }
    }

    class Limit { public string Key; public double Lo, Hi; public bool IsInt, Neg1, Locked; public string Source = ""; }

    // limits.ini (tools/gen_limits.py): <category>.<field path without indices> = min,max,flags   ; source
    class Limits
    {
        public bool Loaded;
        public readonly Dictionary<string, Limit> ByKey = new Dictionary<string, Limit>(StringComparer.Ordinal);
        public readonly HashSet<string> Primary = new HashSet<string>(StringComparer.Ordinal);
        public static readonly string[] Types = { "starshipmovementconfig", "starshipshielddata", "starshiphealthdamagedata",
            "starshippowermanagement", "starshipweaponconfig", "bulletconfig", "missileconfig", "explosionconfig",
            "enginecustom", "hullcustom", "shieldcustom",
            "capitalshiphealthsettings", "damagesectionsettings", "capitalshipsubsystemsettings",
            "capitalshipweakpointssettings", "capitalshipweaponsubsystemsettings", "capitalshipenginewashdamageareasettings",
            "damageovertimesettings", "shieldhealthsettings", "capitalshipresupplysubsystemsettings",
            "turrethealthsettings", "turretaimingsettings", "turrettargetcomputersettings",
            "turretfirepatternsettingsasset" };   // capital ships and turrets (tools/gen_limits.py CAPITAL)

        public static Limits Load(string dir)
        {
            var L = new Limits(); string file = Path.Combine(dir, "limits.ini");
            if (!File.Exists(file)) return L;
            L.Loaded = true;
            foreach (var line in Ini.Section(file, "limits"))
            {
                int eq = line.IndexOf('='); if (eq <= 0 || line[0] == ';') continue;
                string key = new string(line.Substring(0, eq).Where(c => !char.IsWhiteSpace(c)).ToArray()).ToLowerInvariant();
                var m = Regex.Match(line.Substring(eq + 1), @"^\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,([inx]*)");
                double lo, hi;
                if (!m.Success || key.Length == 0 || !Num.TryParse(m.Groups[1].Value, out lo) || !Num.TryParse(m.Groups[2].Value, out hi)) continue;
                string flags = m.Groups[3].Value; int semi = line.IndexOf(';', eq);
                L.ByKey[key] = new Limit { Key = key, Lo = lo, Hi = hi, IsInt = flags.Contains('i'), Neg1 = flags.Contains('n'),
                    Locked = flags.Contains('x'), Source = semi > 0 ? line.Substring(semi + 1).Trim() : "" };
            }
            foreach (var line in Ini.Section(file, "primary"))
            {
                if (line.Length == 0 || line[0] == ';') continue;
                int eq = line.IndexOf('='); string name = eq >= 0 ? line.Substring(0, eq) : line;
                name = new string(name.Where(c => !char.IsWhiteSpace(c)).ToArray()).ToLowerInvariant();
                if (name.Length > 0) L.Primary.Add(name);
            }
            return L;
        }
        // the DLL's find_limit: <type of the asset>.<field without [n]>
        public Limit Find(string asset, string field)
        {
            if (!Loaded) return null;
            string type = Types.FirstOrDefault(t => asset.StartsWith(t, StringComparison.Ordinal));
            if (type == null) return null;
            if (type == "enginecustom" || type == "hullcustom" || type == "shieldcustom") type = "starshipcustomconfig";
            else if (type == "starshipweaponconfig") type += Primary.Contains(asset) ? "_primary" : "_aux";
            string key = type + "." + Regex.Replace(field, @"\[[^\]]*\]", "");
            Limit l; return ByKey.TryGetValue(key, out l) ? l : null;
        }
        public static double Clamp(Limit l, double v)
        {
            if (l == null) return v;
            if (l.Neg1 && v == -1.0) return v;
            if (l.IsInt) v = Num.LlRound(v);
            return v < l.Lo ? l.Lo : v > l.Hi ? l.Hi : v;
        }
    }

    // defaults.ini (written by the DLL): [asset] field=stock value, for every config the mod has seen
    class Defaults
    {
        public readonly SortedDictionary<string, SortedDictionary<string, double>> Assets =
            new SortedDictionary<string, SortedDictionary<string, double>>(StringComparer.Ordinal);
        // each config's name as the game spells it ("; name=HullCustom_Chameleon_Xwing", written by the DLL)
        public readonly Dictionary<string, string> Names = new Dictionary<string, string>(StringComparer.Ordinal);
        public string NameOf(string asset) { string n; return Names.TryGetValue(asset, out n) ? n : null; }
        public static Defaults Load(string dir)
        {
            var d = new Defaults(); string file = Path.Combine(dir, "defaults.ini");
            if (!File.Exists(file)) return d;
            SortedDictionary<string, double> cur = null; string section = null;
            foreach (var raw in File.ReadAllLines(file))
            {
                string line = raw.Trim();
                if (section != null && line.StartsWith("; name=")) { d.Names[section] = line.Substring(7).Trim(); continue; }
                if (line.Length == 0 || line[0] == ';') continue;
                if (line[0] == '[' && line.EndsWith("]")) { string n = section = line.Substring(1, line.Length - 2); if (!d.Assets.TryGetValue(n, out cur)) d.Assets[n] = cur = new SortedDictionary<string, double>(new FieldOrder()); continue; }
                int eq = line.IndexOf('='); double v;
                if (cur != null && eq > 0 && Num.TryParse(line.Substring(eq + 1), out v)) cur[line.Substring(0, eq)] = v;
            }
            return d;
        }
        // fields in a readable order: alphabetical, with array indices in numeric order
        class FieldOrder : IComparer<string>
        {
            public int Compare(string a, string b)
            {
                string pa = Regex.Replace(a, @"\[(\d+)\]", m => "[" + m.Groups[1].Value.PadLeft(4, '0') + "]");
                string pb = Regex.Replace(b, @"\[(\d+)\]", m => "[" + m.Groups[1].Value.PadLeft(4, '0') + "]");
                return string.CompareOrdinal(pa, pb);
            }
        }
    }

    class Profile
    {
        public string Name;                 // "Default" is built in
        public bool NoPinball, NoMultidrift;
        public readonly SortedDictionary<string, double> Values = new SortedDictionary<string, double>(StringComparer.Ordinal);
        public bool BuiltIn { get { return Name == "Default"; } }

        public static bool NameOk(string n)
        {
            if (string.IsNullOrEmpty(n) || n.Contains("..") || n.Length > 60) return false;
            return n.All(c => char.IsLetterOrDigit(c) || c == ' ' || c == '_' || c == '-' || c == '.');
        }
        public static string PathOf(string dir, string name) { return Path.Combine(dir, "profiles", name + ".ini"); }

        // the DLL's load_profile, for one profile file
        public static Profile Load(string dir, string name)
        {
            var p = new Profile { Name = name };
            if (name == "Default") return p;
            string file = PathOf(dir, name);
            if (!File.Exists(file)) return p;
            p.NoPinball = Ini.GetInt(file, "rules", "no_zero_throttle_pinball", 0) != 0;
            p.NoMultidrift = Ini.GetInt(file, "rules", "no_multidrift", 0) != 0;
            foreach (var e in Ini.Section(file, "values"))
            {
                int eq = e.IndexOf('=');
                if (eq < 0 || e[0] == ';' || e[0] == '#') continue;
                string key = new string(e.Substring(0, eq).Where(c => !char.IsWhiteSpace(c)).ToArray()).ToLowerInvariant();
                if (key.Length > 127) key = key.Substring(0, 127);
                double v;
                if (key.Length == 0 || !Num.TryParse(e.Substring(eq + 1), out v)) continue;
                p.Values[key] = v;
            }
            return p;
        }
        public void Save(string dir)
        {
            if (BuiltIn) return;
            Directory.CreateDirectory(Path.Combine(dir, "profiles"));
            var sb = new StringBuilder();
            sb.Append("; Profile '" + Name + "', saved by the settings app (sws_settings.exe). Hand edits are fine too.\r\n");
            sb.Append("[rules]\r\n");
            sb.Append("no_zero_throttle_pinball=" + (NoPinball ? 1 : 0) + "\r\n");
            sb.Append("no_multidrift=" + (NoMultidrift ? 1 : 0) + "\r\n\r\n[values]\r\n");
            foreach (var kv in Values) sb.Append(kv.Key + "=" + Num.G9(kv.Value) + "\r\n");
            File.WriteAllText(PathOf(dir, Name), sb.ToString(), new UTF8Encoding(false));
        }
        public static List<string> List(string dir)
        {
            var names = new List<string> { "Default" };
            string pd = Path.Combine(dir, "profiles");
            if (Directory.Exists(pd))
                names.AddRange(Directory.GetFiles(pd, "*.ini").Select(Path.GetFileNameWithoutExtension)
                    .Where(n => NameOk(n) && !string.Equals(n, "Default", StringComparison.OrdinalIgnoreCase)).OrderBy(n => n, StringComparer.OrdinalIgnoreCase));
            return names;
        }
    }

    // what the game will actually use: values clamped to their limits; locked and unknown fields dropped
    class Effective
    {
        public readonly SortedDictionary<string, double> Values = new SortedDictionary<string, double>(StringComparer.Ordinal);
        public readonly List<string> Notes = new List<string>();
        public uint Fingerprint;

        public static Effective Of(Profile p, Limits limits)
        {
            var e = new Effective();
            foreach (var kv in p.Values)
            {
                int dot = kv.Key.IndexOf('.');
                string asset = dot > 0 ? kv.Key.Substring(0, Math.Min(dot, 95)) : "";
                if (limits.Loaded)
                {
                    Limit l = dot > 0 ? limits.Find(asset, kv.Key.Substring(dot + 1)) : null;
                    if (l == null) { e.Notes.Add(kv.Key + ": no limit (unknown field); not applied"); continue; }
                    if (l.Locked) { e.Notes.Add(kv.Key + ": locked; not applied"); continue; }
                    double c = Limits.Clamp(l, kv.Value);
                    if (c != kv.Value) e.Notes.Add(kv.Key + " = " + Num.Show(kv.Value) + " is outside " + Num.Show(l.Lo) + ".." + Num.Show(l.Hi) + "; the game uses " + Num.Show(c));
                    e.Values[kv.Key] = c;
                }
                else e.Values[kv.Key] = kv.Value;
            }
            e.Fingerprint = Hash(p.NoPinball, p.NoMultidrift, e.Values);
            return e;
        }
        // the DLL's profile_fingerprint: FNV-1a over the rules and the sorted values; 0 = the stock protocol
        static uint Fnv(uint h, string t) { foreach (byte b in Encoding.ASCII.GetBytes(t)) { h ^= b; h *= 16777619u; } return h; }
        public static uint Hash(bool pinball, bool multidrift, SortedDictionary<string, double> values)
        {
            if (!pinball && !multidrift && values.Count == 0) return 0;
            uint h = 2166136261u;
            h = Fnv(h, "pinball=" + (pinball ? 1 : 0) + ";multidrift=" + (multidrift ? 1 : 0) + ";");
            foreach (var kv in values) h = Fnv(h, kv.Key + "=" + Num.G9(kv.Value) + ";");
            return h == 0 ? 1 : h;
        }
        public static string Show(uint fp) { return fp == 0 ? "00000000 (stock)" : fp.ToString("x8"); }
    }

    // the ships and systems the values are grouped by
    static class Catalog
    {
        public static readonly string[] Ships = { "xwing", "ywing", "awing", "uwing", "bwing", "tiefighter", "tiebomber",
            "tieinterceptor", "tiereaper", "tiedefender" };
        static readonly Dictionary<string, string> ShipNames = new Dictionary<string, string> {
            { "xwing", "X-wing" }, { "ywing", "Y-wing" }, { "awing", "A-wing" }, { "uwing", "U-wing" }, { "bwing", "B-wing" },
            { "tiefighter", "TIE Fighter" }, { "tiebomber", "TIE Bomber" }, { "tieinterceptor", "TIE Interceptor" },
            { "tiereaper", "TIE Reaper" }, { "tiedefender", "TIE Defender" } };
        // capital ships, by the name part their configs use (Fleet Battles: MC75, CR90 corvettes and Nebulon-B frigates;
        // Star Destroyer, Raider corvettes and Arquitens light cruisers; the rest appear in other modes)
        public static readonly string[] CapitalShips = { "stardestroyer", "mc75", "corvette", "frigate", "raider",
            "lightcruiser", "starhawk", "quasar", "gozanti", "gr75", "igv55", "zeta", "bff" };
        static readonly Dictionary<string, string> CapitalNames = new Dictionary<string, string> {
            { "stardestroyer", "Star Destroyer" }, { "mc75", "MC75" }, { "corvette", "Corvette (CR90)" },
            { "frigate", "Frigate (Nebulon-B)" }, { "raider", "Raider" }, { "lightcruiser", "Light cruiser (Arquitens)" },
            { "starhawk", "Starhawk" }, { "quasar", "Quasar Fire" }, { "gozanti", "Gozanti" }, { "gr75", "GR-75" },
            { "igv55", "IGV-55" }, { "zeta", "Zeta" }, { "bff", "BFF" } };
        public const string Turrets = "~turrets", Shared = "~shared";
        public static readonly string[] Groups = Ships.Concat(CapitalShips).Concat(new[] { Turrets, Shared }).ToArray();
        public static string ShipName(string s)
        {
            string n;
            if (s != null && (ShipNames.TryGetValue(s, out n) || CapitalNames.TryGetValue(s, out n))) return n;
            return s == Turrets ? "Turrets (shared)" : "Shared";
        }
        public static string CapitalName(string s) { string n; return CapitalNames.TryGetValue(s, out n) ? n : null; }

        static string[] Parts(string asset) { return asset.Split('_'); }
        public static string ShipOf(string asset)
        {
            var parts = Parts(asset);
            foreach (var s in Ships) if (parts.Contains(s)) return s;
            foreach (var s in Ships) if (parts.Any(p => p == s + "ai")) return s;
            foreach (var s in CapitalShips) if (parts.Any(p => p.StartsWith(s, StringComparison.Ordinal))) return s;   // frigatemedium
            if (parts.Any(p => p.StartsWith("turret", StringComparison.Ordinal))) return Turrets;
            return null;
        }
        // AI, bot and single-player variants
        public static bool IsVariant(string asset)
        {
            var parts = Parts(asset);
            return parts.Any(p => p == "ai" || p == "bot" || p == "pvb" || p == "sp" || p == "instalock" || Ships.Any(s => p == s + "ai"));
        }
        public static string SystemOf(string asset, Limits limits)
        {
            if (asset.StartsWith("starshipmovementconfig")) return "Movement";
            if (asset.StartsWith("starshippowermanagement")) return "Power";
            if (asset.StartsWith("starshipshielddata")) return "Shields";
            if (asset.StartsWith("starshiphealthdamagedata")) return "Hull";
            if (asset.StartsWith("starshipweaponconfig") && Parts(asset).Contains("turret")) return "Turret weapons";
            if (asset.StartsWith("starshipweaponconfig")) return limits.Primary.Contains(asset) ? "Primary weapons" : "Aux and countermeasures";
            if (asset.StartsWith("bulletconfig")) return "Laser projectiles";
            if (asset.StartsWith("missileconfig") || asset.StartsWith("explosionconfig")) return "Missiles and explosions";
            if (asset.StartsWith("enginecustom")) return "Engine components";
            if (asset.StartsWith("hullcustom")) return "Hull components";
            if (asset.StartsWith("shieldcustom")) return "Shield components";
            if (asset.StartsWith("capitalshiphealthsettings")) return "Hull";
            if (asset.StartsWith("shieldhealthsettings")) return "Shields";
            if (asset.StartsWith("damagesectionsettings")) return "Damage sections";
            if (asset.StartsWith("capitalshipsubsystemsettings") || asset.StartsWith("capitalshipweaponsubsystemsettings")
                || asset.StartsWith("capitalshipresupplysubsystemsettings")) return "Subsystems";
            if (asset.StartsWith("capitalshipweakpointssettings")) return "Weak points";
            if (asset.StartsWith("turret")) return "Turrets";
            if (asset.StartsWith("capitalshipenginewashdamageareasettings")) return "Engine wash";
            if (asset.StartsWith("damageovertimesettings")) return "Damage over time";
            return "Other";
        }
        public static readonly string[] SystemOrder = { "Movement", "Power", "Shields", "Hull", "Primary weapons",
            "Aux and countermeasures", "Turret weapons", "Laser projectiles", "Missiles and explosions", "Engine components",
            "Hull components", "Shield components", "Damage sections", "Subsystems", "Weak points", "Turrets", "Engine wash",
            "Damage over time", "Other" };
    }

    // server.ini beside the launcher (mod/release/server.ini): the active profile and host/join settings
    class ServerConfig
    {
        public readonly string File;
        public ServerConfig(string dir) { File = Path.Combine(dir, "server.ini"); }
        public bool Exists { get { return System.IO.File.Exists(File); } }
        public string Profile { get { return Ini.Get(File, "server", "profile", "").Trim(); } set { Ini.Set(File, "server", "profile", value); } }
        public string Join { get { return Ini.Get(File, "server", "join", "").Trim(); } set { Ini.Set(File, "server", "join", value); } }
        public string MatchMode { get { return Ini.Get(File, "server", "match_mode", "").Trim(); } set { Ini.Set(File, "server", "match_mode", value); } }
        public int Listen { get { return Ini.GetInt(File, "server", "listen", 0); } }
    }
}
