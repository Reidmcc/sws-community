# Limits

Limits (issue #11) keep every value inside a range the game can take. `limits.ini` beside the DLL gives each field a minimum and maximum. `tools/gen_limits.py` writes it from `defaults.ini`.

## The rule

- **Maximum:** up to 5× the highest value the game itself ships for that field. That's the most its own custom-match modifiers allow.
- **Minimum:** down to 0.1× the lowest. It's 0 where the game ships 0, and for two values where 0 means off: a destroyed weak point's hull damage, and the capital ships' passive damage multiplier.
- **Tighter bounds where they're known:**
  - speeds at most 300 (actual flight speed tops out there, measured);
  - fractions at most 1;
  - angles at most 180.
  
  The passive damage's stop ratio may reach 1, which turns the passive damage off.
- **Components** are percentages: up to 5× the highest stock percentage, down to −0.9, so a value never drops below 10% of the base.
- **Locked:** power levels, and fields the game always ships as 0.

## Out-of-range values

A value outside its range is clamped, with a `limits:` line in the log. The clamped value is the one in the fingerprint.

A locked field, or one with no limit, isn't applied.
