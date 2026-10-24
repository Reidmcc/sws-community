# Ship size

Ship size is one value per starfighter, `shipscale_<ship>.scale` (`shipscale_ywing.scale=0.77`; the
app lists it as **Size** under each ship). It scales the ship's model about its centre as the ship loads, and the laser
hitbox goes with it: the game places each hitbox mesh at its model part, so the hits land on the resized hull (measured
with `hit_log=1`: at 0.5, every AI hit on a Y-wing landed within 0.25 m of the half-size hull). It applies to the
player ship and its AI versions, creep included. The cockpit you sit in keeps its size, so the view from inside doesn't
change. Allowed: 0.5 to 1.5 (0.5 has been flown; larger is untried). The scale is uniform on purpose: stretching one
axis distorts the model, and the game gives the hitbox a single scale anyway.

## Sizes closest to canon

How close to canon to go is a league's call, so here are the scales that match the canon sizes, by length or by
surface area (surface area tracks how much ship there is to hit, averaged over every direction). Game sizes are the
laser hitbox as flown, length / width / height in metres; canon sizes are the New Republic-era figures.

| Ship | Game | Canon | Scale, by length | Scale, by surface area |
|---|---|---|---|---|
| X-wing | 11.9 / 10.2 / 2.1 | 12.5 / 11.8 / 2.4 | 1.05 | 1.10 |
| Y-wing | 23.7 / 12.3 / 2.6 | 18.2 / 8.8 / 2.9 | 0.77 | 0.78 |
| A-wing | 6.5 / 4.5 / 2.2 | 7.7 / 4.6 / 2.0 | 1.19 | 1.07 |
| U-wing (S-foils deployed) | 24.4 / 26.4 / 3.3 | about 19.6 / 20.4 / 3.5 | 0.80 | 0.82 |
| B-wing (long axis) | 16.9 | 16.9 | 1.00 | |
| TIE Fighter | 6.3 / 5.7 / 7.7 | 7.2 / 6.7 / 8.8 | 1.15 | 1.16 |
| TIE Bomber | 8.2 / 10.0 / 5.5 | 7.8 / 9.3 / 5.1 | 0.95 | 0.93 |
| TIE Interceptor | 7.5 / 5.6 / 4.7 | 7.7 / 6.5 / 5.3 | 1.03 | 1.10 |
| TIE Reaper | 33.6 / 23.7 / 5.1 | 24.2 / 16.5 / 4.7 | 0.72 | 0.74 |
| TIE Defender | 11.2 / 10.0 / 8.7 | 9.2 / 10.0 / 8.7 | 0.82 | 0.93 |

The U-wing's canon size with its S-foils deployed is estimated from the S-foils-forward figures (same foil length,
swung back to the game's angle). The Y-wing is the only ship flown at other sizes so far; the other rows are
computed, not flown.

## Setting it

In the settings app: **Profile and values**, open a ship, **Size**, and type the scale. By hand, in the profile's
`[values]`:

```ini
shipscale_ywing.scale=0.77
```

Everyone in a match needs the same sizes: they're part of the profile's fingerprint, so mismatched players are
refused at connect.
