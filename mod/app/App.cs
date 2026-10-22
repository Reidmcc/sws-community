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
        public static readonly Font Body = new Font("Segoe UI", 9.5f), Small = new Font("Segoe UI", 8.5f),
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
        public static string Ask(IWin32Window owner, string title, string prompt, string initial, bool input = true)
        {
            using (var f = new Form { Text = title, FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent,
                MinimizeBox = false, MaximizeBox = false, ShowInTaskbar = false, BackColor = Panel, ForeColor = Text, Font = Body, ClientSize = new Size(440, input ? 150 : 130) })
            {
                var lbl = new Label { Text = prompt, Left = 16, Top = 14, Width = 408, Height = input ? 40 : 60, ForeColor = Text };
                var tb = new TextBox { Left = 16, Top = 58, Width = 408, Text = initial ?? "", BackColor = Field, ForeColor = Text, BorderStyle = BorderStyle.FixedSingle, Visible = input };
                var ok = Button("OK", null, true); var cancel = Button("Cancel", null);
                ok.DialogResult = DialogResult.OK; cancel.DialogResult = DialogResult.Cancel;
                var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, Left = 16, Width = 408, Height = 40, Top = f.ClientSize.Height - 50 };
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
        readonly Button tabPlay, tabValues;
        readonly Panel playPage = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Back, Padding = new Padding(24, 18, 24, 18) };
        readonly Panel valuesPage = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Back, Padding = new Padding(16, 12, 16, 12) };

        // values page
        readonly ComboBox profileBox = Theme.Combo();
        readonly CheckBox rulePinball = Theme.Check("Block zero-throttle pinballing"), ruleMultidrift = Theme.Check("Block multidrifting");
        readonly TextBox search = new TextBox { Width = 230, BackColor = Theme.Field, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle, Font = Theme.Body };
        readonly CheckBox showVariants = Theme.Check("AI and single-player configs");
        readonly CheckBox changedOnly = Theme.Check("Changed only");
        readonly TreeView tree = new TreeView { Dock = DockStyle.Fill, BackColor = Theme.Panel, ForeColor = Theme.Text, BorderStyle = BorderStyle.None,
            LineColor = Theme.Line, Font = Theme.Body, HideSelection = false, FullRowSelect = true, ShowLines = false, ItemHeight = 22 };
        readonly DataGridView grid = new DataGridView { Dock = DockStyle.Fill };
        readonly Label status = Theme.Label("", Theme.Small, Theme.Muted);
        readonly Label readOnlyNote = Theme.Label("", Theme.Small, Theme.Changed);

        // play page
        Button hostToggle, joinToggle; bool joining;
        readonly ComboBox modeBox = Theme.Combo();
        readonly ComboBox addressBox = Theme.Combo(true);
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
            tabPlay = Theme.Button("Play", (s, e) => ShowPage(true)); tabValues = Theme.Button("Profile and values", (s, e) => ShowPage(false));
            left.Controls.Add(tabPlay); left.Controls.Add(tabValues);
            var right = Theme.Row(); right.Dock = DockStyle.Right;
            right.Controls.Add(headerProfile); right.Controls.Add(Theme.Label("fingerprint", Theme.Small, Theme.Muted)); right.Controls.Add(headerFp);
            header.Controls.Add(right); header.Controls.Add(left);

            BuildPlayPage();
            BuildValuesPage();
            Controls.Add(valuesPage); Controls.Add(playPage); Controls.Add(header);
            Load += (s, e) => { Theme.DarkTitle(this); Theme.DarkScroll(tree); Theme.DarkScroll(grid); Reload(); ShowPage(true); };
        }

        public void ShowValuesForShot()
        {
            ShowPage(false);
            var ship = tree.Nodes.Find("xwing", false).FirstOrDefault() ?? (tree.Nodes.Count > 0 ? tree.Nodes[0] : null);
            if (ship != null && ship.Nodes.Count > 0 && ship.Nodes[0].Nodes.Count > 0) { ship.Expand(); ship.Nodes[0].Expand(); tree.SelectedNode = ship.Nodes[0].Nodes[0]; }
        }
        void ShowPage(bool play)
        {
            playPage.Visible = play; valuesPage.Visible = !play;
            tabPlay.BackColor = play ? Theme.Accent : Theme.Field; tabPlay.ForeColor = play ? Color.White : Theme.Text;
            tabValues.BackColor = !play ? Theme.Accent : Theme.Field; tabValues.ForeColor = !play ? Color.White : Theme.Text;
            if (play) RefreshPlay();
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
            rulePinball.Checked = profile.NoPinball; ruleMultidrift.Checked = profile.NoMultidrift;
            rulePinball.Enabled = ruleMultidrift.Enabled = !profile.BuiltIn;
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
            r2.Controls.Add(Theme.Label("Rules", Theme.Bold));
            rulePinball.CheckedChanged += (s, e) => { if (!loading) { profile.NoPinball = rulePinball.Checked; SaveProfile(); } };
            ruleMultidrift.CheckedChanged += (s, e) => { if (!loading) { profile.NoMultidrift = ruleMultidrift.Checked; SaveProfile(); } };
            r2.Controls.Add(rulePinball); r2.Controls.Add(ruleMultidrift);
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
            filter.Controls.Add(showVariants); filter.Controls.Add(changedOnly);
            tree.AfterSelect += (s, e) => FillGrid();
            split.Panel1.Controls.Add(tree); split.Panel1.Controls.Add(filter);
            SetupGrid();
            split.Panel2.Controls.Add(grid);

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
                string ship = Catalog.ShipOf(asset) ?? "~shared";
                string sys = Catalog.SystemOf(asset, limits);
                SortedDictionary<string, List<string>> systems;
                if (!bySpec.TryGetValue(ship, out systems)) bySpec[ship] = systems = new SortedDictionary<string, List<string>>();
                List<string> list; if (!systems.TryGetValue(sys, out list)) systems[sys] = list = new List<string>();
                list.Add(asset);
            }
            TreeNode select = null;
            foreach (var ship in Catalog.Ships.Concat(new[] { "~shared" }))
            {
                SortedDictionary<string, List<string>> systems;
                if (!bySpec.TryGetValue(ship, out systems)) continue;
                var sn = new TreeNode(Catalog.ShipName(ship == "~shared" ? null : ship)) { Name = ship };
                foreach (var sys in Catalog.SystemOrder)
                {
                    List<string> list; if (!systems.TryGetValue(sys, out list)) continue;
                    var yn = new TreeNode(sys);
                    foreach (var a in list)
                    {
                        int n = profile.Values.Keys.Count(k => k.StartsWith(a + ".", StringComparison.Ordinal));
                        var an = new TreeNode(a + (n > 0 ? "  (" + n + ")" : "")) { Tag = a, ForeColor = n > 0 ? Theme.Changed : Theme.Text };
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
                foreach (var a in unknown) un.Nodes.Add(new TreeNode(a) { Tag = a, ForeColor = Theme.Warn });
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
            grid.CellValidating += GridValidating;
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

        IEnumerable<string> SelectedAssets()
        {
            var n = tree.SelectedNode; if (n == null) return Enumerable.Empty<string>();
            var list = new List<string>();
            Action<TreeNode> walk = null; walk = x => { if (x.Tag is string) list.Add((string)x.Tag); foreach (TreeNode c in x.Nodes) walk(c); };
            walk(n); return list;
        }
        void FillGrid()
        {
            grid.Rows.Clear();
            var assets = SelectedAssets().ToList();
            grid.Columns["asset"].Visible = assets.Count != 1;
            string q = search.Text.Trim().ToLowerInvariant();
            foreach (var a in assets)
            {
                SortedDictionary<string, double> fields;
                var keys = defaults.Assets.TryGetValue(a, out fields) ? fields.Keys.ToList() : new List<string>();
                foreach (var k in profile.Values.Keys) if (k.StartsWith(a + ".", StringComparison.Ordinal) && !keys.Contains(k.Substring(a.Length + 1))) keys.Add(k.Substring(a.Length + 1));
                foreach (var f in keys)
                {
                    string key = a + "." + f;
                    if (q.Length > 0 && !f.Contains(q)) continue;
                    double v; bool changed = profile.Values.TryGetValue(key, out v);
                    if (changedOnly.Checked && !changed) continue;
                    double stock = double.NaN; bool known = fields != null && fields.TryGetValue(f, out stock);
                    var l = limits.Find(a, f);
                    string range = !limits.Loaded ? "" : l == null ? "no limit" : l.Locked ? "locked" : Num.Show(l.Lo) + " to " + Num.Show(l.Hi) + (l.Neg1 ? " (or -1)" : "");
                    int r = grid.Rows.Add(a, f, known ? Num.Show(stock) : "?", changed ? Num.Show(v) : "", range, changed ? "reset" : "");
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
        void GridValidating(object sender, DataGridViewCellValidatingEventArgs e)
        {
            if (grid.Columns[e.ColumnIndex].Name != "value" || grid.Rows[e.RowIndex].Cells["value"].ReadOnly) return;
            string key = grid.Rows[e.RowIndex].Tag as string; if (key == null) return;
            string text = (e.FormattedValue ?? "").ToString().Trim();
            double old; bool had = profile.Values.TryGetValue(key, out old);
            if (text.Length == 0) { if (had) { profile.Values.Remove(key); AfterEdit(e.RowIndex, null, null); } return; }
            double v;
            if (!Num.TryParse(text, out v)) { status.Text = "'" + text + "' isn't a number."; e.Cancel = true; return; }
            int dot = key.IndexOf('.'); var l = limits.Find(key.Substring(0, dot), key.Substring(dot + 1));
            double c = Limits.Clamp(l, v);
            if (had && old == c) return;
            profile.Values[key] = c;
            AfterEdit(e.RowIndex, c, c != v ? "outside its allowed range: set to " + Num.Show(c) : null);
        }
        void AfterEdit(int row, double? value, string note)
        {
            SaveProfile();
            BeginInvoke((Action)(() =>
            {
                if (row < grid.Rows.Count)
                {
                    var r = grid.Rows[row];
                    r.Cells["value"].Value = value.HasValue ? Num.Show(value.Value) : "";
                    r.Cells["value"].Style.ForeColor = value.HasValue ? Theme.Changed : Theme.Text;
                    r.Cells["reset"].Value = value.HasValue ? "reset" : "";
                }
                RefreshTreeCounts();
                if (note != null) status.Text = (string)grid.Rows[row].Tag + " " + note + ".";
            }));
        }
        void ResetRow(int row)
        {
            if (profile.BuiltIn || row < 0) return;
            string key = grid.Rows[row].Tag as string;
            if (key != null && profile.Values.Remove(key)) AfterEdit(row, null, null);
        }
        void RefreshTreeCounts()
        {
            Action<TreeNode> walk = null;
            walk = n =>
            {
                foreach (TreeNode c in n.Nodes) walk(c);
                var assets = new List<string>(); Action<TreeNode> collect = null;
                collect = x => { if (x.Tag is string) assets.Add((string)x.Tag); foreach (TreeNode c in x.Nodes) collect(c); }; collect(n);
                int cnt = assets.Sum(a => profile.Values.Keys.Count(k => k.StartsWith(a + ".", StringComparison.Ordinal)));
                string baseText = n.Text; int p = baseText.IndexOf("  ("); if (p > 0) baseText = baseText.Substring(0, p);
                n.Text = baseText + (cnt > 0 ? "  (" + cnt + ")" : ""); n.ForeColor = cnt > 0 ? Theme.Changed : Theme.Text;
            };
            foreach (TreeNode n in tree.Nodes) if (n.Text != "Not in defaults.ini") walk(n);
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
            if (copy) { p.NoPinball = profile.NoPinball; p.NoMultidrift = profile.NoMultidrift; foreach (var kv in profile.Values) p.Values[kv.Key] = kv.Value; }
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
            using (var d = new OpenFileDialog { Title = "Import a profile", Filter = "Profiles (*.ini)|*.ini" })
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
            hostCol.Controls.Add(Theme.Label("In the game, start a solo custom match vs AI: the mod makes it this type, and your friends join it.", Theme.Small, Theme.Muted));
            hostPanel.Controls.Add(hostCol); hostPanel.Margin = new Padding(0, 14, 0, 0);
            col.Controls.Add(hostPanel);

            var joinCol = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Theme.Panel, Dock = DockStyle.Fill };
            joinCol.Controls.Add(Theme.Label("Host's address", Theme.Bold));
            var addrRow = Theme.Row(); addressBox.Width = 260; addrRow.Controls.Add(addressBox);
            addrRow.Controls.Add(Theme.Button("Save as favorite", (s, e) => SaveFavorite(true)));
            addrRow.Controls.Add(Theme.Button("Remove favorite", (s, e) => SaveFavorite(false)));
            joinCol.Controls.Add(addrRow);
            joinCol.Controls.Add(Theme.Label("For example the host's Tailscale address (100.x.y.z). The port is added for you.", Theme.Small, Theme.Muted));
            joinCol.Controls.Add(Theme.Label("In the game, start a solo custom match vs AI: your game joins the host's match instead.", Theme.Small, Theme.Muted));
            joinPanel.Controls.Add(joinCol); joinPanel.Margin = new Padding(0, 14, 0, 0);
            col.Controls.Add(joinPanel);

            var prof = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Color.Transparent, Margin = new Padding(0, 18, 0, 0) };
            prof.Controls.Add(playProfile);
            prof.Controls.Add(Theme.Label("Everyone in a match needs the same fingerprint: the same profile, with the same rules and values.", Theme.Small, Theme.Muted));
            prof.Controls.Add(Theme.Button("Change profile", (s, e) => ShowPage(false)));
            col.Controls.Add(prof);

            var launch = Theme.Button("Launch the game", (s, e) => Launch(), true);
            launch.Margin = new Padding(0, 22, 0, 0); launch.Padding = new Padding(18, 8, 18, 8);
            col.Controls.Add(launch);
            col.Controls.Add(Theme.Label("Steam must be running. The launcher's window asks you to type YES before it starts the game.", Theme.Small, Theme.Muted));
            col.Controls.Add(playWarn);
            playPage.Controls.Add(col);
        }
        void RefreshPlay()
        {
            if (!server.Exists || profile == null) return;
            string join = server.Join;
            SetJoining(join.Length > 0);
            int mi = Array.IndexOf(Modes, server.MatchMode.ToLowerInvariant()); modeBox.SelectedIndex = mi >= 0 ? mi : 0;
            addressBox.Items.Clear(); foreach (var f in Favorites()) addressBox.Items.Add(f);
            addressBox.Text = join;
            playProfile.Text = "Profile: " + profile.Name + "    fingerprint " + Effective.Show(effective != null ? effective.Fingerprint : 0);
            playWarn.Text = server.Listen != 1 ? "server.ini has listen=" + server.Listen + ": hosting needs listen=1 (the release's server.ini has it)." : "";
        }
        void SetJoining(bool join)
        {
            joining = join;
            hostPanel.Visible = !join; joinPanel.Visible = join;
            hostToggle.BackColor = !join ? Theme.Accent : Theme.Field; hostToggle.ForeColor = !join ? Color.White : Theme.Text;
            joinToggle.BackColor = join ? Theme.Accent : Theme.Field; joinToggle.ForeColor = join ? Color.White : Theme.Text;
        }
        string AppIni { get { return Path.Combine(dir, "sws_settings.ini"); } }
        List<string> Favorites() { return Ini.Get(AppIni, "join", "favorites", "").Split(new[] { ';' }, StringSplitOptions.RemoveEmptyEntries).Select(x => x.Trim()).Where(x => x.Length > 0).Distinct().ToList(); }
        void SaveFavorite(bool add)
        {
            string a = addressBox.Text.Trim(); if (a.Length == 0) return;
            var f = Favorites(); f.Remove(a); if (add) f.Insert(0, a);
            Ini.Set(AppIni, "join", "favorites", string.Join(";", f));
            RefreshFavorites(a);
        }
        void RefreshFavorites(string keep) { addressBox.Items.Clear(); foreach (var f in Favorites()) addressBox.Items.Add(f); addressBox.Text = keep; }
        void Launch()
        {
            if (!server.Exists) { Theme.Tell(this, "Launch", "There's no server.ini beside this app."); return; }
            string launcher = Path.Combine(dir, "sws_launch.exe");
            if (!File.Exists(launcher)) { Theme.Tell(this, "Launch", "sws_launch.exe isn't beside this app (" + dir + ")."); return; }
            if (joining)
            {
                string a = addressBox.Text.Trim();
                if (a.Length == 0 || a.Any(c => char.IsWhiteSpace(c) || c == ';')) { Theme.Tell(this, "Join", "Type the host's address first."); return; }
                server.Join = a;
            }
            else
            {
                server.Join = "";
                server.MatchMode = Modes[Math.Max(0, modeBox.SelectedIndex)];
            }
            server.Profile = profile.Name;
            try { Process.Start(new ProcessStartInfo(launcher) { WorkingDirectory = dir, UseShellExecute = true }); }
            catch (Exception ex) { Theme.Tell(this, "Launch", "Couldn't start the launcher: " + ex.Message); }
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
                    if (args.Length >= 3 && args[2] == "values") form.ShowValuesForShot();
                    Application.DoEvents();
                    using (var bmp = new Bitmap(form.Width, form.Height)) { form.DrawToBitmap(bmp, new Rectangle(Point.Empty, form.Size)); bmp.Save(args[1]); }
                    form.Close();
                };
            }
            Application.Run(form);
        }
    }
}
