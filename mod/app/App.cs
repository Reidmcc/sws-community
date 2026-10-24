// The settings app (issues #4, #5): host or join, and profiles of rules and changed values, in a dark theme.
// It lives beside the launcher and the DLL and only reads and writes files there (server.ini, profiles\, its own
// sws_settings.ini); it never touches the game folder. The model and everything that must match the DLL is in Core.cs.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace SwsSettings
{
    static class Theme
    {
        public static readonly Color Back = Color.FromArgb(30, 31, 34), Panel = Color.FromArgb(43, 45, 49),
            Field = Color.FromArgb(56, 58, 64), Line = Color.FromArgb(70, 73, 80), Text = Color.FromArgb(230, 232, 235),
            Muted = Color.FromArgb(150, 156, 165), Accent = Color.FromArgb(90, 140, 240), Changed = Color.FromArgb(232, 178, 80),
            Warn = Color.FromArgb(229, 100, 90), Good = Color.FromArgb(110, 200, 140);
        public static readonly Font Body = new Font("Segoe UI", 9.5f), Small = new Font("Segoe UI", 9.5f),
            Bold = new Font("Segoe UI", 9.5f, FontStyle.Bold), Title = new Font("Segoe UI Semibold", 13f), Big = new Font("Segoe UI Semibold", 12f);

        [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] static extern int SetWindowTheme(IntPtr hwnd, string app, string idList);
        public static void DarkTitle(Form f) { try { int on = 1; if (DwmSetWindowAttribute(f.Handle, 20, ref on, 4) != 0) DwmSetWindowAttribute(f.Handle, 19, ref on, 4); } catch { } }
        public static void DarkScroll(Control c) { try { SetWindowTheme(c.Handle, "DarkMode_Explorer", null); } catch { } }

        public static Button Button(string text, EventHandler click, bool primary = false)
        {
            var b = new Button { Text = text, FlatStyle = FlatStyle.Flat, BackColor = primary ? Accent : Field, ForeColor = primary ? Color.White : Text,
                Font = primary ? Big : Body, AutoSize = true, Padding = new Padding(8, 2, 8, 2), Margin = new Padding(0, 0, 6, 0), Cursor = Cursors.Hand, UseVisualStyleBackColor = false };
            b.FlatAppearance.BorderColor = primary ? Accent : Line;
            b.FlatAppearance.MouseOverBackColor = primary ? ControlPaint.Light(Accent, 0.15f) : Line;
            if (click != null) b.Click += click;
            return b;
        }
        // a page or mode button: blue while active, and it stays blue under the mouse; inactive ones keep the grey hover
        public static void SetActive(Button b, bool active)
        {
            b.BackColor = active ? Accent : Field; b.ForeColor = active ? Color.White : Text;
            b.FlatAppearance.BorderColor = active ? Accent : Line;
            b.FlatAppearance.MouseOverBackColor = active ? Accent : Line;
            b.FlatAppearance.MouseDownBackColor = active ? Accent : Line;
        }
        public static Label Label(string text, Font font = null, Color? color = null)
        {
            return new Label { Text = text, AutoSize = true, ForeColor = color ?? Text, Font = font ?? Body, BackColor = Color.Transparent, Margin = new Padding(0, 6, 8, 0) };
        }
        public static ComboBox Combo(bool editable = false)
        {
            var c = new ComboBox { DropDownStyle = editable ? ComboBoxStyle.DropDown : ComboBoxStyle.DropDownList, FlatStyle = FlatStyle.Flat,
                BackColor = Field, ForeColor = Text, Font = Body, Margin = new Padding(0, 2, 8, 0) };
            if (!editable)
            {
                c.DrawMode = DrawMode.OwnerDrawFixed;
                c.DrawItem += (s, e) =>
                {
                    if (e.Index < 0) return;
                    bool sel = (e.State & DrawItemState.Selected) != 0;
                    using (var bg = new SolidBrush(sel ? Accent : Field)) e.Graphics.FillRectangle(bg, e.Bounds);
                    TextRenderer.DrawText(e.Graphics, c.Items[e.Index].ToString(), c.Font, e.Bounds, sel ? Color.White : Text, TextFormatFlags.VerticalCenter | TextFormatFlags.Left);
                };
            }
            return c;
        }
        public static CheckBox Check(string text) { return new DarkCheck { Text = text, Font = Body, Margin = new Padding(0, 4, 18, 0) }; }
        public static FlowLayoutPanel Row(int top = 0) { return new FlowLayoutPanel { AutoSize = true, WrapContents = false, BackColor = Color.Transparent, Margin = new Padding(0, top, 0, 0), Padding = new Padding(0) }; }

        // a small dark dialog: an optional text box, OK and Cancel
        public static string Ask(IWin32Window owner, string title, string prompt, string initial, bool input = true, string okText = "OK", int height = 0)
        {
            using (var f = new Form { Text = title, FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent,
                MinimizeBox = false, MaximizeBox = false, ShowInTaskbar = false, BackColor = Panel, ForeColor = Text, Font = Body, ClientSize = new Size(height > 0 ? 520 : 440, height > 0 ? height : input ? 150 : 130) })
            {
                var lbl = new Label { Text = prompt, Left = 16, Top = 14, Width = f.ClientSize.Width - 32, Height = input ? 40 : f.ClientSize.Height - 70, ForeColor = Text };
                var tb = new TextBox { Left = 16, Top = 58, Width = 408, Text = initial ?? "", BackColor = Field, ForeColor = Text, BorderStyle = BorderStyle.FixedSingle, Visible = input };
                var ok = Button(okText, null, true); var cancel = Button("Cancel", null);
                ok.DialogResult = DialogResult.OK; cancel.DialogResult = DialogResult.Cancel;
                var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, Left = 16, Width = f.ClientSize.Width - 32, Height = 40, Top = f.ClientSize.Height - 50 };
                buttons.Controls.Add(cancel); buttons.Controls.Add(ok);
                f.Controls.AddRange(new Control[] { lbl, tb, buttons });
                f.AcceptButton = ok; f.CancelButton = cancel;
                f.Load += (s, e) => { DarkTitle(f); if (input) { tb.Focus(); tb.SelectAll(); } };
                return f.ShowDialog(owner) == DialogResult.OK ? (input ? tb.Text.Trim() : "yes") : null;
            }
        }
        public static void Tell(IWin32Window owner, string title, string text) { Ask(owner, title, text, null, false); }
    }

    // a checkbox drawn for the dark theme: a dark box, an accent fill and check mark when checked
    class DarkCheck : CheckBox
    {
        public DarkCheck() { SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true); AutoSize = false; Cursor = Cursors.Hand; }
        protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); FitSize(); }
        protected override void OnFontChanged(EventArgs e) { base.OnFontChanged(e); FitSize(); }
        void FitSize() { var t = TextRenderer.MeasureText(Text, Font); Size = new Size(t.Width + 26, Math.Max(t.Height, 16) + 4); }
        protected override void OnPaint(PaintEventArgs e)
        {
            Color bg = Theme.Back;   // the nearest ancestor with a real (not transparent) background
            for (Control p = Parent; p != null; p = p.Parent) if (p.BackColor.A == 255) { bg = p.BackColor; break; }
            var g = e.Graphics; g.Clear(bg);
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            var box = new Rectangle(1, (Height - 16) / 2, 15, 15);
            using (var fill = new SolidBrush(Checked ? Theme.Accent : Theme.Field)) g.FillRectangle(fill, box);
            using (var pen = new Pen(Checked ? Theme.Accent : Theme.Muted)) g.DrawRectangle(pen, box);
            if (Checked)
                using (var pen = new Pen(Color.White, 2f))
                    g.DrawLines(pen, new[] { new Point(box.Left + 3, box.Top + 8), new Point(box.Left + 6, box.Top + 11), new Point(box.Left + 12, box.Top + 4) });
            TextRenderer.DrawText(g, Text, Font, new Rectangle(22, 0, Width - 22, Height), Enabled ? Theme.Text : Theme.Muted, TextFormatFlags.VerticalCenter | TextFormatFlags.Left);
        }
    }

    class MainForm : Form
    {
        readonly string dir = Path.GetDirectoryName(Application.ExecutablePath);
        readonly ServerConfig server;
        Limits limits; Defaults defaults;
        Profile profile; Effective effective;
        bool loading;

        // header
        readonly Label headerProfile = Theme.Label("", Theme.Body), headerFp = Theme.Label("", Theme.Bold, Theme.Accent);
        readonly Button tabPlay, tabRules, tabValues;
        const int PagePlay = 0, PageRules = 1, PageValues = 2;
        readonly Panel playPage = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Back, Padding = new Padding(24, 18, 24, 18) };
        readonly Panel valuesPage = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Back, Padding = new Padding(16, 12, 16, 12) };
        readonly Panel rulesPage = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Back, Padding = new Padding(24, 18, 24, 18) };

        // rules page: the profile's yes/no rules and fixes, each off by default (the game as shipped)
        readonly CheckBox ruleZavian = Theme.Check("Zavian Abyss: full middle X-wing creep pack");
        readonly ComboBox[,] creepBoxes = new ComboBox[2, 3];
        readonly CheckBox creepAggressive = Theme.Check("Aggressive creep that keep to the phases");
        readonly CheckBox creepMissiles = Theme.Check("Creep fire missiles and torpedoes");
        static readonly string[][] CreepShipNames = { new[] { "X-wing (stock)", "Y-wing", "A-wing", "U-wing" },
            new[] { "TIE fighter (stock)", "TIE bomber", "TIE interceptor", "TIE reaper" } };
        readonly Label rulesProfile = Theme.Label("", Theme.Big), rulesNote = Theme.Label("", Theme.Small, Theme.Changed);

        // values page
        readonly ComboBox profileBox = Theme.Combo();
        readonly CheckBox rulePinball = Theme.Check("Block zero-throttle pinballing"), ruleMultidrift = Theme.Check("Block multidrifting");
        readonly TextBox search = new TextBox { Width = 230, BackColor = Theme.Field, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle, Font = Theme.Body };
        readonly CheckBox showVariants = Theme.Check("AI and single-player configs");
        readonly CheckBox recordCheck = Theme.Check("Record flight data for testing");
        readonly CheckBox changedOnly = Theme.Check("Changed only");
        readonly CheckBox showLocked = Theme.Check("Show locked values");
        readonly TreeView tree = new TreeView { Dock = DockStyle.Fill, BackColor = Theme.Panel, ForeColor = Theme.Text, BorderStyle = BorderStyle.None,
            LineColor = Theme.Line, Font = Theme.Body, HideSelection = false, FullRowSelect = true, ShowLines = false, ItemHeight = 22, ShowNodeToolTips = true };
        readonly DataGridView grid = new DataGridView { Dock = DockStyle.Fill };
        readonly Label status = Theme.Label("", Theme.Small, Theme.Muted);
        readonly Label readOnlyNote = Theme.Label("", Theme.Small, Theme.Changed);
        readonly Label pickHint = new Label { Dock = DockStyle.Fill, AutoSize = false, TextAlign = ContentAlignment.MiddleCenter, ForeColor = Theme.Muted,
            Font = Theme.Body, BackColor = Theme.Back, Visible = false };

        // play page
        Button hostToggle, joinToggle; bool joining;
        readonly ComboBox modeBox = Theme.Combo();
        readonly TextBox addressBox = new TextBox { Width = 260, BackColor = Theme.Field, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle, Font = Theme.Body, Margin = new Padding(0, 3, 8, 0) };
        readonly ComboBox serversBox = Theme.Combo();
        readonly List<KeyValuePair<string, string>> servers = new List<KeyValuePair<string, string>>();
        readonly Label tsAddress = Theme.Label("", Theme.Big, Theme.Accent), tsNote = Theme.Label("", Theme.Small, Theme.Muted);
        Button tsCopy;
        readonly Label launchStatus = Theme.Label("", Theme.Body, Theme.Muted);
        readonly Panel hostPanel = new Panel { AutoSize = true, BackColor = Theme.Panel, Padding = new Padding(16) };
        readonly Panel joinPanel = new Panel { AutoSize = true, BackColor = Theme.Panel, Padding = new Padding(16) };
        readonly Label playProfile = Theme.Label("", Theme.Body), playWarn = Theme.Label("", Theme.Small, Theme.Warn);
        static readonly string[] Modes = { "fleet", "coop", "solo", "dogfight" };
        static readonly string[] ModeNames = { "Fleet Battles (PvP)", "Co-op vs AI", "Solo vs AI", "Dogfight" };

        public MainForm()
        {
            server = new ServerConfig(dir);
            Text = "Squadrons community mod";
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = new Size(1180, 760); MinimumSize = new Size(900, 560); StartPosition = FormStartPosition.CenterScreen;

            // header: title, the two pages, and the active profile with its fingerprint
            var header = new Panel { Dock = DockStyle.Top, Height = 58, BackColor = Theme.Panel, Padding = new Padding(16, 12, 16, 8) };
            var left = Theme.Row(); left.Dock = DockStyle.Left;
            left.Controls.Add(new Label { Text = "Squadrons community mod", AutoSize = true, Font = Theme.Title, ForeColor = Theme.Text, Margin = new Padding(0, 2, 28, 0) });
            tabPlay = Theme.Button("Play", (s, e) => ShowPage(PagePlay)); tabRules = Theme.Button("Rules and fixes", (s, e) => ShowPage(PageRules));
            tabValues = Theme.Button("Profile and values", (s, e) => ShowPage(PageValues));
            left.Controls.Add(tabPlay); left.Controls.Add(tabRules); left.Controls.Add(tabValues);
            var right = Theme.Row(); right.Dock = DockStyle.Right;
            right.Controls.Add(headerProfile); right.Controls.Add(Theme.Label("fingerprint", Theme.Small, Theme.Muted)); right.Controls.Add(headerFp);
            header.Controls.Add(right); header.Controls.Add(left);

            BuildPlayPage();
            BuildRulesPage();
            BuildValuesPage();
            Controls.Add(valuesPage); Controls.Add(rulesPage); Controls.Add(playPage); Controls.Add(header);
            Load += (s, e) => { Theme.DarkTitle(this); Theme.DarkScroll(tree); Theme.DarkScroll(grid); Reload(); ShowPage(PagePlay); };
        }

        // --test-edit: type <text> into a value cell and press Enter, the way the grid handles it, then report what stuck
        public string TestEdit(string asset, string field, string text)
        {
            ShowPage(PageValues);
            TreeNode target = null; Action<TreeNode> find = null;
            find = n => { if (target == null && (n.Tag as string) == asset) target = n; foreach (TreeNode c in n.Nodes) find(c); };
            foreach (TreeNode n in tree.Nodes) find(n);
            if (target == null) return "no tree node for " + asset;
            tree.SelectedNode = target;
            DataGridViewRow row = grid.Rows.Cast<DataGridViewRow>().FirstOrDefault(r => (r.Tag as string) == asset + "." + field);
            if (row == null) return "no row for " + asset + "." + field;
            var sw = Stopwatch.StartNew();
            grid.Focus();
            grid.CurrentCell = row.Cells["value"];
            bool began = grid.BeginEdit(true);
            if (grid.EditingControl != null) grid.EditingControl.Text = text;
            var enter = typeof(DataGridView).GetMethod("ProcessEnterKey", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
            object handled = enter != null ? enter.Invoke(grid, new object[] { Keys.Enter }) : (object)grid.EndEdit();
            for (int i = 0; i < 20; i++) { Application.DoEvents(); System.Threading.Thread.Sleep(10); }
            long ms = sw.ElapsedMilliseconds;
            DataGridViewRow after = grid.Rows.Cast<DataGridViewRow>().FirstOrDefault(r => (r.Tag as string) == asset + "." + field);
            double v; bool inProfile = profile.Values.TryGetValue(asset + "." + field, out v);
            string file = File.Exists(Profile.PathOf(dir, profile.Name)) ? File.ReadAllText(Profile.PathOf(dir, profile.Name)) : "";
            return "began edit " + began + ", enter handled " + handled + ", " + ms + " ms\r\n" +
                   "cell now '" + (after != null ? after.Cells["value"].Value : "(row gone)") + "', in edit mode " + grid.IsCurrentCellInEditMode + "\r\n" +
                   "profile value " + (inProfile ? Num.G9(v) : "(none)") + ", in file " + file.Contains(asset + "." + field + "=") + "\r\n" +
                   "status '" + status.Text + "'";
        }
        public void ShowValuesForShot(string mode)
        {
            ShowPage(PageValues);
            var ship = tree.Nodes.Find(mode == "match" ? Catalog.Match : "xwing", false).FirstOrDefault() ?? (tree.Nodes.Count > 0 ? tree.Nodes[0] : null);
            if (ship == null) return;
            ship.Expand();
            if (mode == "ship") { tree.SelectedNode = ship; foreach (TreeNode sys in ship.Nodes) if (sys.Text.StartsWith("Engine") || sys.Text.StartsWith("Primary")) sys.Expand(); return; }
            if (ship.Nodes.Count > 0) tree.SelectedNode = ship.Nodes[0];   // Movement: one config, shown directly
        }
        public void ShowRulesForShot() { ShowPage(PageRules); }
        void ShowPage(int page)
        {
            playPage.Visible = page == PagePlay; rulesPage.Visible = page == PageRules; valuesPage.Visible = page == PageValues;
            Theme.SetActive(tabPlay, page == PagePlay); Theme.SetActive(tabRules, page == PageRules); Theme.SetActive(tabValues, page == PageValues);
            if (page == PagePlay) RefreshPlay();
        }

        // ---------------------------------------------------------------------------------------------- data
        void Reload()
        {
            limits = Limits.Load(dir);
            defaults = Defaults.Load(dir);
            if (!server.Exists) Theme.Tell(this, "server.ini not found", "There's no server.ini beside this app (" + dir + "). Keep the app in the mod's folder, next to sws_launch.exe.");
            string active = server.Profile;
            if (active.Length == 0 || !Profile.NameOk(active) || (active != "Default" && !File.Exists(Profile.PathOf(dir, active)))) active = "Default";
            FillProfiles(active);
        }
        void FillProfiles(string select)
        {
            loading = true;
            profileBox.Items.Clear();
            foreach (var n in Profile.List(dir)) profileBox.Items.Add(n);
            int i = profileBox.Items.IndexOf(select); profileBox.SelectedIndex = i >= 0 ? i : 0;
            loading = false;
            SelectProfile((string)profileBox.SelectedItem, false);
        }
        void SelectProfile(string name, bool makeActive)
        {
            profile = Profile.Load(dir, name);
            if (makeActive && server.Exists) server.Profile = name;
            loading = true;
            rulePinball.Checked = profile.NoPinball; ruleMultidrift.Checked = profile.NoMultidrift; ruleZavian.Checked = profile.FixZavianCreep;
            rulePinball.Enabled = ruleMultidrift.Enabled = ruleZavian.Enabled = !profile.BuiltIn;
            for (int f = 0; f < 2; f++) for (int k = 0; k < 3; k++) { creepBoxes[f, k].SelectedIndex = profile.Creep[f, k]; creepBoxes[f, k].Enabled = !profile.BuiltIn; }
            creepAggressive.Checked = profile.CreepAggressive; creepMissiles.Checked = profile.CreepMissiles;
            creepAggressive.Enabled = creepMissiles.Enabled = !profile.BuiltIn;
            rulesProfile.Text = "Rules and fixes in profile " + profile.Name;
            rulesNote.Text = profile.BuiltIn ? "Default is the game as shipped: every rule and fix off. Duplicate it on Profile and values to make your own." : "";
            loading = false;
            readOnlyNote.Text = profile.BuiltIn ? "Default is the game as shipped and can't be changed. Duplicate it to make your own profile."
                : "Type in Your value to change a value; clear it, press Delete or click reset to go back to the default. Changes are saved as you go and apply at the next launch.";
            readOnlyNote.ForeColor = profile.BuiltIn ? Theme.Changed : Theme.Muted;
            Recompute();
            BuildTree();
        }
        void Recompute()
        {
            effective = Effective.Of(profile, limits);
            string fp = Effective.Show(effective.Fingerprint);
            headerProfile.Text = "Profile: " + profile.Name;
            headerFp.Text = fp;
            int changed = profile.Values.Count;
            status.Text = (limits.Loaded ? "" : "limits.ini is missing: values won't be checked against their limits. ") +
                (defaults.Assets.Count == 0 ? "defaults.ini is missing: launch the game once with the mod to list the values. " : "") +
                changed + " changed value" + (changed == 1 ? "" : "s") + (effective.Notes.Count > 0 ? ". " + string.Join(" ", effective.Notes.Take(3)) : "");
            RefreshPlay();
        }
        void SaveProfile()
        {
            if (profile.BuiltIn) return;
            try { profile.Save(dir); } catch (Exception ex) { Theme.Tell(this, "Couldn't save", ex.Message); }
            Recompute();
        }

        // ---------------------------------------------------------------------------------------------- values page
        void BuildValuesPage()
        {
            var top = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Theme.Back, Padding = new Padding(0, 0, 0, 8) };
            var r1 = Theme.Row();
            r1.Controls.Add(Theme.Label("Profile"));
            profileBox.Width = 220;
            profileBox.SelectedIndexChanged += (s, e) => { if (!loading) SelectProfile((string)profileBox.SelectedItem, true); };
            r1.Controls.Add(profileBox);
            r1.Controls.Add(Theme.Button("New", (s, e) => NewProfile(false)));
            r1.Controls.Add(Theme.Button("Duplicate", (s, e) => NewProfile(true)));
            r1.Controls.Add(Theme.Button("Rename", (s, e) => RenameProfile()));
            r1.Controls.Add(Theme.Button("Delete", (s, e) => DeleteProfile()));
            r1.Controls.Add(Theme.Button("Import", (s, e) => ImportProfile()));
            r1.Controls.Add(Theme.Button("Open folder", (s, e) => { Directory.CreateDirectory(Path.Combine(dir, "profiles")); Process.Start("explorer.exe", "\"" + Path.Combine(dir, "profiles") + "\""); }));
            var r2 = Theme.Row(6);
            r2.Controls.Add(Theme.Button("Reset all values to default", (s, e) => ResetAll()));
            top.Controls.Add(r1); top.Controls.Add(r2); top.Controls.Add(readOnlyNote);

            var bottom = new Panel { Dock = DockStyle.Bottom, Height = 26, BackColor = Theme.Back };
            status.Dock = DockStyle.Fill; status.AutoSize = false; status.AutoEllipsis = true; bottom.Controls.Add(status);

            var split = new SplitContainer { Dock = DockStyle.Fill, BackColor = Theme.Line, SplitterWidth = 4, FixedPanel = FixedPanel.Panel1 };
            split.Panel1.BackColor = Theme.Panel; split.Panel2.BackColor = Theme.Back;
            var filter = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Theme.Panel, Padding = new Padding(8) };
            filter.Controls.Add(Theme.Label("Search fields", Theme.Small, Theme.Muted));
            search.TextChanged += (s, e) => FillGrid();
            filter.Controls.Add(search);
            showVariants.CheckedChanged += (s, e) => BuildTree();
            changedOnly.CheckedChanged += (s, e) => { BuildTree(); FillGrid(); };
            showLocked.CheckedChanged += (s, e) => FillGrid();
            filter.Controls.Add(showVariants); filter.Controls.Add(changedOnly); filter.Controls.Add(showLocked);
            tree.AfterSelect += (s, e) => FillGrid();
            split.Panel1.Controls.Add(tree); split.Panel1.Controls.Add(filter);
            SetupGrid();
            split.Panel2.Controls.Add(grid);
            split.Panel2.Controls.Add(pickHint);

            valuesPage.Controls.Add(split); valuesPage.Controls.Add(bottom); valuesPage.Controls.Add(top);
            valuesPage.Layout += (s, e) => { if (split.Width > 400 && split.SplitterDistance != 300) try { split.SplitterDistance = 300; } catch { } };
        }

        void BuildTree()
        {
            if (defaults == null) return;
            string keep = tree.SelectedNode != null ? (tree.SelectedNode.Tag as string ?? tree.SelectedNode.FullPath) : null;
            tree.BeginUpdate(); tree.Nodes.Clear();
            var bySpec = new SortedDictionary<string, SortedDictionary<string, List<string>>>();
            foreach (var asset in defaults.Assets.Keys)
            {
                if (!showVariants.Checked && Catalog.IsVariant(asset)) continue;
                if (changedOnly.Checked && !profile.Values.Keys.Any(k => k.StartsWith(asset + ".", StringComparison.Ordinal))) continue;
                string ship = Catalog.ShipOf(asset) ?? Catalog.Shared;
                string sys = Catalog.SystemOf(asset, limits);
                SortedDictionary<string, List<string>> systems;
                if (!bySpec.TryGetValue(ship, out systems)) bySpec[ship] = systems = new SortedDictionary<string, List<string>>();
                List<string> list; if (!systems.TryGetValue(sys, out list)) systems[sys] = list = new List<string>();
                list.Add(asset);
            }
            TreeNode select = null;
            foreach (var ship in Catalog.Groups)
            {
                SortedDictionary<string, List<string>> systems;
                if (!bySpec.TryGetValue(ship, out systems)) continue;
                var sn = new TreeNode(Catalog.ShipName(ship)) { Name = ship };
                foreach (var sys in Catalog.SystemOrder)
                {
                    List<string> list; if (!systems.TryGetValue(sys, out list)) continue;
                    var yn = new TreeNode(sys);
                    foreach (var a in list)
                    {
                        int n = profile.Values.Keys.Count(k => k.StartsWith(a + ".", StringComparison.Ordinal));
                        var an = new TreeNode(ConfigName(a) + (n > 0 ? "  (" + n + ")" : "")) { Tag = a, ForeColor = n > 0 ? Theme.Changed : Theme.Text, ToolTipText = ConfigTip(a) };
                        yn.Nodes.Add(an);
                        if (keep == a) select = an;
                    }
                    int sc = list.Sum(a => profile.Values.Keys.Count(k => k.StartsWith(a + ".", StringComparison.Ordinal)));
                    if (sc > 0) { yn.Text += "  (" + sc + ")"; yn.ForeColor = Theme.Changed; }
                    sn.Nodes.Add(yn);
                }
                tree.Nodes.Add(sn);
            }
            // a profile's values for configs this game hasn't recorded (yet): still listed, so they can be seen and reset
            var unknown = profile.Values.Keys.Select(k => k.Split('.')[0]).Distinct().Where(a => !defaults.Assets.ContainsKey(a)).ToList();
            if (unknown.Count > 0)
            {
                var un = new TreeNode("Not in defaults.ini") { ForeColor = Theme.Warn };
                foreach (var a in unknown) un.Nodes.Add(new TreeNode(ConfigName(a)) { Tag = a, ForeColor = Theme.Warn, ToolTipText = ConfigTip(a) });
                tree.Nodes.Add(un);
            }
            if (tree.Nodes.Count > 0 && select == null && defaults.Assets.Count > 0) { var x = tree.Nodes.Find("xwing", false); tree.Nodes[x.Length > 0 ? tree.Nodes.IndexOf(x[0]) : 0].Expand(); }
            tree.EndUpdate();
            if (select != null) { tree.SelectedNode = select; select.EnsureVisible(); }
            FillGrid();
        }

        void SetupGrid()
        {
            grid.BackgroundColor = Theme.Back; grid.GridColor = Theme.Line; grid.BorderStyle = BorderStyle.None;
            grid.EnableHeadersVisualStyles = false; grid.RowHeadersVisible = false; grid.AllowUserToAddRows = false; grid.AllowUserToDeleteRows = false;
            grid.AllowUserToResizeRows = false; grid.SelectionMode = DataGridViewSelectionMode.CellSelect; grid.MultiSelect = false;
            grid.EditMode = DataGridViewEditMode.EditOnEnter;   // one click on Your value starts typing (Pseudo)
            grid.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill; grid.ColumnHeadersHeightSizeMode = DataGridViewColumnHeadersHeightSizeMode.DisableResizing;
            grid.ColumnHeadersHeight = 30; grid.RowTemplate.Height = 24; grid.CellBorderStyle = DataGridViewCellBorderStyle.SingleHorizontal;
            grid.ColumnHeadersDefaultCellStyle = new DataGridViewCellStyle { BackColor = Theme.Panel, ForeColor = Theme.Muted, Font = Theme.Small, SelectionBackColor = Theme.Panel, Padding = new Padding(4, 0, 0, 0) };
            grid.DefaultCellStyle = new DataGridViewCellStyle { BackColor = Theme.Back, ForeColor = Theme.Text, Font = Theme.Body, SelectionBackColor = Color.FromArgb(52, 66, 96), SelectionForeColor = Theme.Text, Padding = new Padding(4, 0, 4, 0) };
            grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "asset", HeaderText = "Config", ReadOnly = true, FillWeight = 120 });
            grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "field", HeaderText = "Field", ReadOnly = true, FillWeight = 190 });
            grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "stock", HeaderText = "Default", ReadOnly = true, FillWeight = 60 });
            grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "value", HeaderText = "Your value", FillWeight = 60 });
            grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "range", HeaderText = "Allowed", ReadOnly = true, FillWeight = 80 });
            grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "reset", HeaderText = "", ReadOnly = true, FillWeight = 32 });
            grid.Columns["value"].DefaultCellStyle = new DataGridViewCellStyle { BackColor = Theme.Panel, ForeColor = Theme.Text, Font = Theme.Bold };
            grid.Columns["reset"].DefaultCellStyle = new DataGridViewCellStyle { ForeColor = Theme.Accent, Font = Theme.Small, Alignment = DataGridViewContentAlignment.MiddleCenter };
            grid.CellBeginEdit += (s2, e2) => { editing = true; };
            grid.CellValidating += GridValidating;
            grid.CellEndEdit += GridEndEdit;
            grid.CellClick += (s, e) => { if (e.RowIndex >= 0 && grid.Columns[e.ColumnIndex].Name == "reset" && (string)grid.Rows[e.RowIndex].Cells["reset"].Value == "reset") ResetRow(e.RowIndex); };
            grid.KeyDown += (s, e) => { if (e.KeyCode == Keys.Delete && grid.CurrentCell != null && !grid.IsCurrentCellInEditMode) { ResetRow(grid.CurrentCell.RowIndex); e.Handled = true; } };
            grid.CellToolTipTextNeeded += (s, e) =>
            {
                if (e.RowIndex < 0) return;
                var key = grid.Rows[e.RowIndex].Tag as string; if (key == null) return;
                int dot = key.IndexOf('.'); var l = limits.Find(key.Substring(0, dot), key.Substring(dot + 1));
                e.ToolTipText = key + (l != null ? "\n" + l.Source : "\n(no limit known: the game won't apply a value here)");
            };
            grid.DataError += (s, e) => { e.ThrowException = false; };
        }

        // a config's name in the tree: the short, distinctive part ("Agile"); hovering shows the full name and the key
        string ConfigName(string asset) { return Words.Config(asset, true, defaults.NameOf(asset)); }
        string ConfigTip(string asset) { return Words.Config(asset, false, defaults.NameOf(asset)) + "\n" + asset; }
        IEnumerable<string> SelectedAssets()
        {
            var n = tree.SelectedNode; if (n == null) return Enumerable.Empty<string>();
            if (n.Tag is string) return new[] { (string)n.Tag };
            if (n.Nodes.Count == 1 && n.Nodes[0].Tag is string) return new[] { (string)n.Nodes[0].Tag };
            return Enumerable.Empty<string>();
        }
        void FillGrid()
        {
            grid.Rows.Clear();
            var assets = SelectedAssets().ToList();
            grid.Columns["asset"].Visible = false;
            pickHint.Visible = assets.Count == 0; grid.Visible = assets.Count > 0;
            if (assets.Count == 0)
                pickHint.Text = tree.SelectedNode == null ? "Pick a config on the left to see its values."
                    : tree.SelectedNode.Level == 0 ? "Open " + tree.SelectedNode.Text + " and pick a system or a config to see its values."
                    : "Pick one of the configs under " + tree.SelectedNode.Text + " to see its values.";
            string q = search.Text.Trim().ToLowerInvariant();
            foreach (var a in assets)
            {
                SortedDictionary<string, double> fields;
                var keys = defaults.Assets.TryGetValue(a, out fields) ? fields.Keys.ToList() : new List<string>();
                foreach (var k in profile.Values.Keys) if (k.StartsWith(a + ".", StringComparison.Ordinal) && !keys.Contains(k.Substring(a.Length + 1))) keys.Add(k.Substring(a.Length + 1));
                foreach (var f in keys)
                {
                    string key = a + "." + f;
                    string pretty = Words.Pretty(f);
                    if (q.Length > 0 && !f.Contains(q.Replace(" ", "")) && pretty.IndexOf(q, StringComparison.OrdinalIgnoreCase) < 0) continue;
                    double v; bool changed = profile.Values.TryGetValue(key, out v);
                    if (changedOnly.Checked && !changed) continue;
                    double stock = double.NaN; bool known = fields != null && fields.TryGetValue(f, out stock);
                    var l = limits.Find(a, f);
                    // locked fields, and fields with no limit (the game won't apply them either), only on request;
                    // a value the profile does set for one stays visible, so it can be seen and reset
                    if (!showLocked.Checked && !changed && limits.Loaded && (l == null || l.Locked)) continue;
                    string range = !limits.Loaded ? "" : l == null ? "no limit" : l.Locked ? "locked"
                        : l.Source.StartsWith("yes/no") ? "0 = no, 1 = yes"
                        : Num.Show(l.Lo) + " to " + Num.Show(l.Hi) + (l.Neg1 ? " (or -1)" : "");
                    int r = grid.Rows.Add(a, pretty, known ? Num.Show(stock) : "?", changed ? Num.Show(v) : "", range, changed ? "reset" : "");
                    var row = grid.Rows[r]; row.Tag = key;
                    bool locked = profile.BuiltIn || (limits.Loaded && (l == null || l.Locked));
                    row.Cells["value"].ReadOnly = locked;
                    if (locked) row.Cells["value"].Style.BackColor = Theme.Back;
                    if (changed) row.Cells["value"].Style.ForeColor = Theme.Changed;
                    if (l != null && l.Locked) row.Cells["range"].Style.ForeColor = Theme.Muted;
                    if (limits.Loaded && l == null) row.Cells["range"].Style.ForeColor = Theme.Warn;
                }
            }
        }
        // An edit only changes the profile; the cell is redrawn from the profile when the edit ends (GridEndEdit), so what
        // the grid shows is always what is saved. Invalid input is never refused in place (that kept the cell from letting
        // go, which froze the window): the edit ends and the cell shows the profile's value again, with a note.
        bool editing; string pendingNote;
        void GridValidating(object sender, DataGridViewCellValidatingEventArgs e)
        {
            if (!editing || grid.Columns[e.ColumnIndex].Name != "value" || grid.Rows[e.RowIndex].Cells["value"].ReadOnly) return;
            string key = grid.Rows[e.RowIndex].Tag as string; if (key == null) return;
            string text = (e.FormattedValue ?? "").ToString().Trim();
            double old; bool had = profile.Values.TryGetValue(key, out old);
            if (had && text == Num.Show(old)) return;   // unchanged (the cell shows a rounded form of the saved value)
            if (text.Length == 0) { if (had) { profile.Values.Remove(key); Changed(key + " is back to its default."); } return; }
            double v;
            if (!Num.TryParse(text, out v)) { pendingNote = "'" + text + "' isn't a number; " + key + " is unchanged."; return; }
            int dot = key.IndexOf('.'); var l = limits.Find(key.Substring(0, dot), key.Substring(dot + 1));
            double c = Limits.Clamp(l, v);
            int row = e.RowIndex;
            if (c != v)   // on the grid's last row Enter commits without leaving the cell, so show what was saved
                BeginInvoke((Action)(() => { if (row < grid.Rows.Count) { grid.EndEdit(); RenderRow(grid.Rows[row]); } }));
            if (had && old == c) return;
            profile.Values[key] = c;
            Changed(c != v ? key + " = " + Num.Show(v) + " is outside its allowed range: set to " + Num.Show(c) + "." : null);
        }
        void GridEndEdit(object sender, DataGridViewCellEventArgs e)
        {
            editing = false;
            if (e.RowIndex >= 0 && e.RowIndex < grid.Rows.Count) RenderRow(grid.Rows[e.RowIndex]);
            if (pendingNote != null) { status.Text = pendingNote; pendingNote = null; }
        }
        void RenderRow(DataGridViewRow r)
        {
            string key = r.Tag as string; if (key == null) return;
            double v; bool changed = profile.Values.TryGetValue(key, out v);
            r.Cells["value"].Value = changed ? Num.Show(v) : "";
            r.Cells["value"].Style.ForeColor = changed ? Theme.Changed : Theme.Text;
            r.Cells["reset"].Value = changed ? "reset" : "";
        }
        void Changed(string note)
        {
            SaveProfile();                              // also recomputes the fingerprint and the status line
            if (note != null) pendingNote = note;
            BeginInvoke((Action)RefreshTreeCounts);
        }
        void ResetRow(int row)
        {
            if (profile.BuiltIn || row < 0 || row >= grid.Rows.Count) return;
            string key = grid.Rows[row].Tag as string;
            if (key == null || !profile.Values.Remove(key)) return;
            SaveProfile(); RenderRow(grid.Rows[row]); RefreshTreeCounts();
            status.Text = key + " is back to its default.";
        }
        // the changed-value counts on the tree's nodes: counted once, and only nodes whose count changes are touched
        void RefreshTreeCounts()
        {
            var perAsset = new Dictionary<string, int>(StringComparer.Ordinal);
            foreach (var k in profile.Values.Keys) { string a = k.Split('.')[0]; int n; perAsset.TryGetValue(a, out n); perAsset[a] = n + 1; }
            Func<TreeNode, int> walk = null;
            walk = node =>
            {
                int cnt = 0, own;
                if (node.Tag is string && perAsset.TryGetValue((string)node.Tag, out own)) cnt += own;
                foreach (TreeNode c in node.Nodes) cnt += walk(c);
                string baseText = node.Text; int p = baseText.IndexOf("  ("); if (p > 0) baseText = baseText.Substring(0, p);
                string text = baseText + (cnt > 0 ? "  (" + cnt + ")" : "");
                Color color = cnt > 0 ? Theme.Changed : Theme.Text;
                if (node.Text != text) node.Text = text;
                if (node.ForeColor != color) node.ForeColor = color;
                return cnt;
            };
            tree.BeginUpdate();
            try { foreach (TreeNode n in tree.Nodes) if (!n.Text.StartsWith("Not in defaults.ini")) walk(n); }
            finally { tree.EndUpdate(); }
        }
        void ResetAll()
        {
            if (profile.BuiltIn || profile.Values.Count == 0) return;
            if (Theme.Ask(this, "Reset all values", "Remove all " + profile.Values.Count + " changed values from '" + profile.Name + "'? Its rules stay as they are.", null, false) == null) return;
            profile.Values.Clear(); SaveProfile(); BuildTree();
        }

        string AskName(string title, string initial)
        {
            for (;;)
            {
                string n = Theme.Ask(this, title, "Profile name (letters, digits, spaces, _ - .):", initial);
                if (n == null) return null;
                if (!Profile.NameOk(n) || string.Equals(n, "Default", StringComparison.OrdinalIgnoreCase)) { Theme.Tell(this, title, "'" + n + "' can't be used as a profile name."); initial = n; continue; }
                if (File.Exists(Profile.PathOf(dir, n))) { Theme.Tell(this, title, "There's already a profile called '" + n + "'."); initial = n; continue; }
                return n;
            }
        }
        void NewProfile(bool copy)
        {
            string n = AskName(copy ? "Duplicate profile" : "New profile", copy ? profile.Name + " copy" : "");
            if (n == null) return;
            var p = new Profile { Name = n };
            if (copy)
            {
                p.NoPinball = profile.NoPinball; p.NoMultidrift = profile.NoMultidrift; p.FixZavianCreep = profile.FixZavianCreep;
                for (int f = 0; f < 2; f++) for (int k = 0; k < 3; k++) p.Creep[f, k] = profile.Creep[f, k];
                p.CreepAggressive = profile.CreepAggressive; p.CreepMissiles = profile.CreepMissiles;
                foreach (var kv in profile.Values) p.Values[kv.Key] = kv.Value;
            }
            p.Save(dir);
            if (server.Exists) server.Profile = n;
            FillProfiles(n);
        }
        void RenameProfile()
        {
            if (profile.BuiltIn) { Theme.Tell(this, "Rename", "Default is built in and can't be renamed."); return; }
            string n = AskName("Rename profile", profile.Name); if (n == null) return;
            File.Move(Profile.PathOf(dir, profile.Name), Profile.PathOf(dir, n));
            if (server.Exists && server.Profile == profile.Name) server.Profile = n;
            FillProfiles(n);
        }
        void DeleteProfile()
        {
            if (profile.BuiltIn) { Theme.Tell(this, "Delete", "Default is built in and can't be deleted."); return; }
            if (Theme.Ask(this, "Delete profile", "Move '" + profile.Name + "' to the Recycle Bin?", null, false) == null) return;
            Microsoft.VisualBasic.FileIO.FileSystem.DeleteFile(Profile.PathOf(dir, profile.Name), Microsoft.VisualBasic.FileIO.UIOption.OnlyErrorDialogs,
                Microsoft.VisualBasic.FileIO.RecycleOption.SendToRecycleBin);
            if (server.Exists && server.Profile == profile.Name) server.Profile = "Default";
            FillProfiles("Default");
        }
        void ImportProfile()
        {
            Directory.CreateDirectory(Path.Combine(dir, "profiles"));
            using (var d = new OpenFileDialog { Title = "Import a profile", Filter = "Profiles (*.ini)|*.ini", InitialDirectory = Path.Combine(dir, "profiles") })
            {
                if (d.ShowDialog(this) != DialogResult.OK) return;
                string n = Path.GetFileNameWithoutExtension(d.FileName);
                if (!Profile.NameOk(n) || n.Equals("Default", StringComparison.OrdinalIgnoreCase) || File.Exists(Profile.PathOf(dir, n)))
                    n = AskName("Import profile", Profile.NameOk(n) ? n + " (imported)" : "Imported");
                if (n == null) return;
                Directory.CreateDirectory(Path.Combine(dir, "profiles"));
                File.Copy(d.FileName, Profile.PathOf(dir, n));
                if (server.Exists) server.Profile = n;
                FillProfiles(n);
            }
        }

        // ---------------------------------------------------------------------------------------------- rules page
        // The profile's yes/no rules: fixes for the game's exploits and for mistakes in its maps. Each is off by default, which
        // keeps the game as it shipped: players are used to its flaws, and keeping one is their choice.
        void BuildRulesPage()
        {
            var col = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true, BackColor = Theme.Back };
            var head = Theme.Row(); head.Controls.Add(rulesProfile);
            head.Controls.Add(Theme.Button("Change profile", (s, e) => ShowPage(PageValues)));
            col.Controls.Add(head);
            col.Controls.Add(Theme.Label("Each is off unless you turn it on, which keeps the game as it shipped. They're saved in the profile, with its values, and apply at the next launch.", Theme.Small, Theme.Muted));
            col.Controls.Add(rulesNote);

            Action<string, string> section = (title, text) =>
            {
                var t = Theme.Label(title, Theme.Bold); t.Margin = new Padding(0, 22, 0, 2); col.Controls.Add(t);
                if (text != null) col.Controls.Add(Theme.Label(text, Theme.Small, Theme.Muted));
            };
            Action<CheckBox, string> rule = (box, text) =>
            {
                box.Margin = new Padding(0, 12, 0, 0); col.Controls.Add(box);
                var d = Theme.Label(text, Theme.Small, Theme.Muted); d.Margin = new Padding(22, 0, 0, 0); d.MaximumSize = new Size(820, 0); col.Controls.Add(d);
            };
            section("Exploits", "Everyone in a match needs the same setting for these: they're part of the profile's fingerprint.");
            rule(rulePinball, "Boosting from below cruise speed (zero-throttle pinballing) ramps up to boost speed, the way it does from full throttle, instead of jumping straight there. Boosting while already above cruise speed is unchanged.");
            rule(ruleMultidrift, "A drift can start only while the afterburner is on, so each boost allows one drift, as with the combined boost-and-drift input. Releasing and re-pressing drift mid-drift no longer starts a new drift.");
            section("Map fixes", "These are part of the fingerprint too, so everyone in a match knows the server plays the map the way they expect.");
            rule(ruleZavian, "In Zavian Abyss's data, one of the middle X-wing creep pack's two spawn points is told to return control instead of to spawn, so the pack has 3 ships instead of 5, at the start and on every respawn. This makes it spawn all 5.");

            section("Creep packs", "Each faction has three AI creep packs of 5 ships. Pick each pack's ship: Y-wings and TIE bombers bomb capital ships when the phase brings them close. " +
                "Changed packs get creep durability (about a quarter of the player ship's hull, half its shields) and fly lasers only, plus bombs on the bombers, unless creep missiles are on below. " +
                "Packs are numbered along the line they spawn in. Part of the fingerprint.");
            for (int f = 0; f < 2; f++)
            {
                var row = Theme.Row(6); row.Margin = new Padding(0, 10, 0, 0);
                row.Controls.Add(new Label { Text = f == 0 ? "Rebel" : "Imperial", AutoSize = true, Font = Theme.Body, ForeColor = Theme.Text, Margin = new Padding(0, 6, 12, 0), MinimumSize = new Size(70, 0) });
                for (int k = 0; k < 3; k++)
                {
                    var box = Theme.Combo(); box.Width = 170; foreach (var n in CreepShipNames[f]) box.Items.Add(n);
                    int ff = f, kk = k;
                    box.SelectedIndexChanged += (s, e) => { if (!loading && box.SelectedIndex >= 0) { profile.Creep[ff, kk] = box.SelectedIndex; SaveProfile(); } };
                    row.Controls.Add(new Label { Text = "Pack " + (k + 1), AutoSize = true, Font = Theme.Small, ForeColor = Theme.Muted, Margin = new Padding(k == 0 ? 0 : 14, 8, 6, 0) });
                    row.Controls.Add(box); creepBoxes[f, k] = box;
                }
                col.Controls.Add(row);
            }
            rule(creepAggressive, "Creep engage enemies within 2,000 m, as the vs-AI bots do, instead of only what their path brings near. Their targets follow the phase: " +
                "on offense the enemy frigates, then the flagship once the frigates are gone; in the opener the enemy corvette or Raider; on defense no capital ship. " +
                "Players and other creep stay targets throughout.");
            rule(creepMissiles, "Creep fire their missiles, and the bombers their torpedoes, as the vs-AI bots do (stock creep never do). This also has creep dogfight the way the bots do, " +
                "chasing a target's tail. Applies to every pack of both factions.");

            creepAggressive.CheckedChanged += (s, e) => { if (!loading) { profile.CreepAggressive = creepAggressive.Checked; SaveProfile(); } };
            creepMissiles.CheckedChanged += (s, e) => { if (!loading) { profile.CreepMissiles = creepMissiles.Checked; SaveProfile(); } };
            rulePinball.CheckedChanged += (s, e) => { if (!loading) { profile.NoPinball = rulePinball.Checked; SaveProfile(); } };
            ruleMultidrift.CheckedChanged += (s, e) => { if (!loading) { profile.NoMultidrift = ruleMultidrift.Checked; SaveProfile(); } };
            ruleZavian.CheckedChanged += (s, e) => { if (!loading) { profile.FixZavianCreep = ruleZavian.Checked; SaveProfile(); } };
            rulesPage.Controls.Add(col);
        }

        // ---------------------------------------------------------------------------------------------- play page
        void BuildPlayPage()
        {
            var col = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true, BackColor = Theme.Back };
            var toggles = Theme.Row();
            hostToggle = Theme.Button("Host a match", (s, e) => SetJoining(false)); joinToggle = Theme.Button("Join a match", (s, e) => SetJoining(true));
            hostToggle.Font = joinToggle.Font = Theme.Big; hostToggle.Padding = joinToggle.Padding = new Padding(14, 4, 14, 4);
            toggles.Controls.Add(hostToggle); toggles.Controls.Add(joinToggle);
            col.Controls.Add(toggles);

            var hostCol = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Theme.Panel, Dock = DockStyle.Fill };
            hostCol.Controls.Add(Theme.Label("Match type", Theme.Bold));
            modeBox.Width = 260; foreach (var m in ModeNames) modeBox.Items.Add(m);
            hostCol.Controls.Add(modeBox);
            hostCol.Controls.Add(Theme.Label("In the game, create a custom match with the mode set to solo vs AI: the mod makes it the selected type, and your friends join it.", Theme.Small, Theme.Muted));
            var tsTitle = Theme.Label("Your Tailscale address: give it to the players joining you", Theme.Bold); tsTitle.Margin = new Padding(0, 18, 8, 0);
            hostCol.Controls.Add(tsTitle);
            var tsRow = Theme.Row(); tsAddress.Margin = new Padding(0, 4, 12, 0);
            tsCopy = Theme.Button("Copy", (s, e) => { if (tsAddress.Text.Length > 0) { try { Clipboard.SetText(tsAddress.Text); tsNote.Text = "Copied " + tsAddress.Text + " to the clipboard."; } catch { } } });
            tsRow.Controls.Add(tsAddress); tsRow.Controls.Add(tsCopy);
            hostCol.Controls.Add(tsRow); hostCol.Controls.Add(tsNote);
            hostPanel.Controls.Add(hostCol); hostPanel.Margin = new Padding(0, 14, 0, 0);
            col.Controls.Add(hostPanel);

            var joinCol = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Theme.Panel, Dock = DockStyle.Fill };
            joinCol.Controls.Add(Theme.Label("Saved servers", Theme.Bold));
            var savedRow = Theme.Row(); serversBox.Width = 320; savedRow.Controls.Add(serversBox);
            serversBox.SelectedIndexChanged += (s, e) => { int i = serversBox.SelectedIndex; if (i >= 0 && i < servers.Count) addressBox.Text = servers[i].Value; };
            savedRow.Controls.Add(Theme.Button("Remove", (s, e) => RemoveFavorite()));
            joinCol.Controls.Add(savedRow);
            var addrTitle = Theme.Label("Host's address", Theme.Bold); addrTitle.Margin = new Padding(0, 14, 8, 0);
            joinCol.Controls.Add(addrTitle);
            var addrRow = Theme.Row(); addrRow.Controls.Add(addressBox);
            addrRow.Controls.Add(Theme.Button("Save as favorite...", (s, e) => SaveFavorite()));
            joinCol.Controls.Add(addrRow);
            joinCol.Controls.Add(Theme.Label("For example the host's Tailscale address (100.x.y.z). The port is added for you.", Theme.Small, Theme.Muted));
            joinCol.Controls.Add(Theme.Label("In the game, start a solo custom match vs AI: your game joins the host's match instead.", Theme.Small, Theme.Muted));
            joinPanel.Controls.Add(joinCol); joinPanel.Margin = new Padding(0, 14, 0, 0);
            col.Controls.Add(joinPanel);

            var prof = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Color.Transparent, Margin = new Padding(0, 18, 0, 0) };
            prof.Controls.Add(playProfile);
            prof.Controls.Add(Theme.Label("Everyone in a match needs the same fingerprint: the same profile, with the same rules and values.", Theme.Small, Theme.Muted));
            prof.Controls.Add(Theme.Button("Change profile", (s, e) => ShowPage(PageValues)));
            col.Controls.Add(prof);

            recordCheck.Margin = new Padding(0, 18, 0, 0);
            recordCheck.CheckedChanged += (s, e) => { if (server.Exists && server.Record != recordCheck.Checked) server.Record = recordCheck.Checked; };
            col.Controls.Add(recordCheck);
            col.Controls.Add(Theme.Label("Only for testing changes to the mod: it records every ship's flight data (30-50 MB a match, in the logs folder) and uses some CPU. Leave it off to play.", Theme.Small, Theme.Muted));
            var launch = Theme.Button("Launch the game", (s, e) => Launch(), true);
            launch.Margin = new Padding(0, 22, 0, 0); launch.Padding = new Padding(18, 8, 18, 8);
            col.Controls.Add(launch);
            col.Controls.Add(Theme.Label("Steam must be running. You'll be asked to confirm before the game starts.", Theme.Small, Theme.Muted));
            col.Controls.Add(launchStatus);
            col.Controls.Add(playWarn);
            playPage.Controls.Add(col);
        }
        void RefreshPlay()
        {
            if (!server.Exists || profile == null) return;
            string join = server.Join;
            SetJoining(join.Length > 0);
            int mi = Array.IndexOf(Modes, server.MatchMode.ToLowerInvariant()); modeBox.SelectedIndex = mi >= 0 ? mi : 0;
            addressBox.Text = join;
            RefreshServers(join);
            recordCheck.Checked = server.Record;
            string ts = TailscaleAddress();
            tsAddress.Text = ts ?? ""; tsCopy.Visible = ts != null;
            tsNote.Text = ts != null ? "Port 25200 is added for them. Everyone needs Tailscale, with this PC shared to them in Tailscale's admin page."
                : "Tailscale isn't running on this PC, or isn't installed (tailscale.com). Without it, players need another way to reach this PC: UDP port 25200.";
            playProfile.Text = "Profile: " + profile.Name + "    fingerprint " + Effective.Show(effective != null ? effective.Fingerprint : 0);
            playWarn.Text = server.Listen != 1 ? "server.ini has listen=" + server.Listen + ": hosting needs listen=1 (the release's server.ini has it)." : "";
        }
        void SetJoining(bool join)
        {
            joining = join;
            hostPanel.Visible = !join; joinPanel.Visible = join;
            Theme.SetActive(hostToggle, !join); Theme.SetActive(joinToggle, join);
        }
        string AppIni { get { return Path.Combine(dir, "sws_settings.ini"); } }
        // saved servers: [servers] name=address in sws_settings.ini. Unnamed favorites from the first version
        // ([join] favorites=a;b) are carried over, named by their address.
        List<KeyValuePair<string, string>> LoadServers()
        {
            var legacy = Ini.Get(AppIni, "join", "favorites", "").Split(new[] { ';' }, StringSplitOptions.RemoveEmptyEntries).Select(x => x.Trim()).Where(x => x.Length > 0).ToList();
            if (legacy.Count > 0)
            {
                foreach (var a in legacy) if (Ini.Get(AppIni, "servers", a, "").Length == 0) Ini.Set(AppIni, "servers", a, a);
                Ini.Set(AppIni, "join", "favorites", null);
            }
            var list = new List<KeyValuePair<string, string>>();
            foreach (var line in Ini.Section(AppIni, "servers"))
            {
                int eq = line.IndexOf('='); if (eq <= 0 || line[0] == ';') continue;
                string name = line.Substring(0, eq).Trim(), addr = line.Substring(eq + 1).Trim();
                if (name.Length > 0 && addr.Length > 0) list.Add(new KeyValuePair<string, string>(name, addr));
            }
            return list.OrderBy(kv => kv.Key, StringComparer.OrdinalIgnoreCase).ToList();
        }
        void RefreshServers(string selectAddress)
        {
            servers.Clear(); servers.AddRange(LoadServers());
            serversBox.Items.Clear();
            foreach (var kv in servers) serversBox.Items.Add(kv.Key == kv.Value ? kv.Key : kv.Key + "   (" + kv.Value + ")");
            serversBox.Enabled = servers.Count > 0;
            if (servers.Count == 0) { serversBox.Items.Add("No saved servers yet"); serversBox.SelectedIndex = 0; return; }
            int i = servers.FindIndex(kv => kv.Value == selectAddress);
            serversBox.SelectedIndex = i;   // -1 leaves the address box as it is
        }
        void SaveFavorite()
        {
            string addr = addressBox.Text.Trim();
            if (addr.Length == 0) { Theme.Tell(this, "Save a server", "Type the host's address first."); return; }
            var existing = LoadServers().FirstOrDefault(kv => kv.Value == addr);
            string name = Theme.Ask(this, "Save a server", "A name for " + addr + ", for example \"Pseudo's server\":", existing.Key ?? "");
            if (name == null) return;
            name = name.Trim();
            if (name.Length == 0 || name.IndexOfAny(new[] { '=', ';', '[', ']' }) >= 0) { Theme.Tell(this, "Save a server", "Names can't be empty or contain = ; [ ]"); return; }
            foreach (var kv in LoadServers()) if (kv.Value == addr || string.Equals(kv.Key, name, StringComparison.OrdinalIgnoreCase)) Ini.Set(AppIni, "servers", kv.Key, null);
            Ini.Set(AppIni, "servers", name, addr);
            RefreshServers(addr);
        }
        void RemoveFavorite()
        {
            int i = serversBox.SelectedIndex;
            if (i < 0 || i >= servers.Count) return;
            var kv = servers[i];
            if (Theme.Ask(this, "Remove server", "Remove \"" + kv.Key + "\" (" + kv.Value + ") from your saved servers?", null, false) == null) return;
            Ini.Set(AppIni, "servers", kv.Key, null);
            RefreshServers(addressBox.Text.Trim());
        }
        // this PC's Tailscale address: Tailscale gives every device one in 100.64.0.0/10, on its own network adapter
        static string TailscaleAddress()
        {
            try
            {
                string best = null;
                foreach (var ni in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (ni.OperationalStatus != System.Net.NetworkInformation.OperationalStatus.Up) continue;
                    bool named = (ni.Name + " " + ni.Description).IndexOf("tailscale", StringComparison.OrdinalIgnoreCase) >= 0;
                    foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                    {
                        if (ua.Address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork) continue;
                        var b = ua.Address.GetAddressBytes();
                        if (b[0] != 100 || b[1] < 64 || b[1] > 127) continue;
                        if (named) return ua.Address.ToString();
                        if (best == null) best = ua.Address.ToString();
                    }
                }
                return best;
            }
            catch { return null; }
        }
        // Launch: the consent the launcher asked for in its console (type YES) is a popup here; the launcher then runs
        // hidden with --yes, which skips only that question: its checks (EasyAntiCheat, the game already running, files
        // missing) all come first and still refuse, and what it says is shown if it doesn't start the game.
        void Launch()
        {
            if (!server.Exists) { Theme.Tell(this, "Launch", "There's no server.ini beside this app."); return; }
            string launcher = Path.Combine(dir, "sws_launch.exe");
            if (!File.Exists(launcher)) { Theme.Tell(this, "Launch", "sws_launch.exe isn't beside this app (" + dir + ")."); return; }
            if (joining)
            {
                string a = addressBox.Text.Trim();
                if (a.Length == 0 || a.Any(c => char.IsWhiteSpace(c) || c == ';')) { Theme.Tell(this, "Join", "Type the host's address first."); return; }
            }
            string what = joining ? "join " + addressBox.Text.Trim() : "host " + ModeNames[Math.Max(0, modeBox.SelectedIndex)];
            if (Theme.Ask(this, "Launch the game",
                "This starts Squadrons directly, without EasyAntiCheat, with the community server mod loaded from the start, to " +
                what + " with the profile '" + profile.Name + "'.\n\nThe game's EasyAntiCheat DLL loads but stays inactive, as with any direct launch. " +
                "Loading a mod into the game carries some risk to your EA account; we think it's low, but nobody can promise it's zero.",
                null, false, "Launch", 210) == null) return;
            ApplyPlaySettings();
            launchStatus.ForeColor = Theme.Muted; launchStatus.Text = "Starting...";
            var t = new System.Threading.Thread(() =>
            {
                int code; string output = RunLauncher(launcher, out code);
                BeginInvoke((Action)(() => ShowLaunchResult(code, output)));
            }) { IsBackground = true };
            t.Start();
        }
        void ApplyPlaySettings()
        {
            if (joining) server.Join = addressBox.Text.Trim();
            else { server.Join = ""; server.MatchMode = Modes[Math.Max(0, modeBox.SelectedIndex)]; }
            server.Profile = profile.Name;
        }
        string RunLauncher(string launcher, out int code)
        {
            try
            {
                var psi = new ProcessStartInfo(launcher, "--yes") { WorkingDirectory = dir, UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true };
                // read as it comes and stop when the launcher exits: the game it starts may keep the output pipe open,
                // so waiting for the pipe to close could wait for the game
                var text = new System.Text.StringBuilder();
                DataReceivedEventHandler take = (s, e) => { if (e.Data != null) lock (text) text.AppendLine(e.Data); };
                using (var p = new Process { StartInfo = psi })
                {
                    p.OutputDataReceived += take; p.ErrorDataReceived += take;
                    p.Start(); p.StandardInput.Close();
                    p.BeginOutputReadLine(); p.BeginErrorReadLine();
                    bool done = p.WaitForExit(60000);
                    System.Threading.Thread.Sleep(300);   // the last lines arrive just after exit
                    lock (text)
                    {
                        if (!done) { try { p.Kill(); } catch { } code = -1; return text + "\n(the launcher didn't finish within a minute)"; }
                        code = p.ExitCode;
                        return text.ToString();
                    }
                }
            }
            catch (Exception ex) { code = -1; return "Couldn't run the launcher: " + ex.Message; }
        }
        void ShowLaunchResult(int code, string output)
        {
            var lines = output.Replace("\r", "").Split('\n').Select(x => x.Trim()).Where(x => x.Length > 0 && !x.StartsWith("Community server mod launcher")).ToList();
            string note = string.Join(" ", lines.Where(x => x.StartsWith("Note:")));
            if (code == 0 && lines.Any(x => x.StartsWith("Started pid")))
            {
                launchStatus.ForeColor = Theme.Good;
                launchStatus.Text = "The game is starting." + (note.Length > 0 ? " " + note : "");
                return;
            }
            launchStatus.ForeColor = Theme.Warn;
            launchStatus.Text = "The game wasn't started.";
            Theme.Ask(this, "The game wasn't started", string.Join("\n", lines.DefaultIfEmpty("The launcher stopped without saying why (exit code " + code + ").")), null, false, "OK", 220);
        }
        // --test-launch: the launch path after the popup (settings, hidden launcher, its output), reported to a file
        public string TestLaunch()
        {
            ApplyPlaySettings();
            int code; string output = RunLauncher(Path.Combine(dir, "sws_launch.exe"), out code);
            return "exit " + code + "\r\n" + output;
        }
    }

    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            var form = new MainForm();
            // --shot <png> [values]: render the window to an image and exit (for checking the layout; writes nothing else)
            if (args.Length >= 2 && args[0] == "--shot")
            {
                form.StartPosition = FormStartPosition.Manual; form.Location = new Point(-4000, -4000); form.ShowInTaskbar = false;
                form.Shown += (s, e) =>
                {
                    if (args.Length >= 3 && args[2].StartsWith("values")) form.ShowValuesForShot(args[2] == "values-ship" ? "ship" : args[2] == "values-match" ? "match" : "system");
                    if (args.Length >= 3 && args[2] == "rules") form.ShowRulesForShot();
                    Application.DoEvents();
                    using (var bmp = new Bitmap(form.Width, form.Height)) { form.DrawToBitmap(bmp, new Rectangle(Point.Empty, form.Size)); bmp.Save(args[1]); }
                    form.Close();
                };
            }
            // --test-launch <report>: the launch path after the confirmation popup, reported to a file
            if (args.Length >= 2 && args[0] == "--test-launch")
            {
                form.StartPosition = FormStartPosition.Manual; form.Location = new Point(-4000, -4000); form.ShowInTaskbar = false;
                form.Shown += (s, e) => { string r; try { r = form.TestLaunch(); } catch (Exception ex) { r = "EXCEPTION " + ex; } File.WriteAllText(args[1], r); form.Close(); };
            }
            // --test-edit <report> <asset> <field> <text>: one cell edit as the grid does it, reported to a file
            if (args.Length >= 5 && args[0] == "--test-edit")
            {
                form.StartPosition = FormStartPosition.Manual; form.Location = new Point(-4000, -4000); form.ShowInTaskbar = false;
                form.Shown += (s, e) => { string r; try { r = form.TestEdit(args[2], args[3], args[4]); } catch (Exception ex) { r = "EXCEPTION " + ex; } File.WriteAllText(args[1], r); form.Close(); };
            }
            Application.Run(form);
        }
    }
}
