# Profiles and the fingerprint

Everyone in a match must use the same profile.

A fingerprint of the profile, made from the rules plus every value (normalized), is part of the network version. So players on different profiles are refused at connect. The log's `profile:` line shows it, and the settings app shows it in its header, computed the same way.

`profile=Default` is built in: no rules, no changes, and the game's own network version.

With no `profile=` line, the older `rules.ini` is read instead. Missing rules count as on.

The release ships these profiles in its `profiles` folder:
- **AsIntended:** both exploits removed.
- **NoMultidrift:** pinballing allowed, multidrifting removed.
- **Bomber Escort Fleet Battle:** a ready-to-play mode where only the bomber creep sink capital ships ([players-vs-capital-ships.md](players-vs-capital-ships.md)).
