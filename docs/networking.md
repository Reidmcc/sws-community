# Networking and Tailscale

## Why a network path is needed

A normal Squadrons match runs through EA's servers, which also get the players connected to each other. The mod cuts EA out: the host's own PC runs the match server, on UDP port 25200. So every joiner's game has to reach the host's PC directly, and home routers don't let unknown outsiders in.

There are two ways around that:
- **Tailscale** (the tested way, [tailscale.com](https://tailscale.com), free): every player installs it, and each PC gets a private address starting with 100. Tailscale connects the PCs directly through their routers, encrypted, or relays the traffic through its own servers when a direct path isn't possible. To the game it works like a LAN.
- **Port forwarding:** the host tells their router to send UDP port 25200 to their PC. That works without Tailscale, but it opens the port to the whole internet, and every joiner needs the host's home IP address.

Nothing in the mod depends on Tailscale. Anything that gets joiners to UDP port 25200 on the host's PC works.

## Setting it up

1. Everyone installs Tailscale and signs in.
2. The host shares their PC with each joiner: in Tailscale's admin console, **Machines**, then the PC's **...** menu, then **Share**. Each joiner accepts the invite.
3. The host gives joiners their Tailscale address (100.x.y.z). The settings app's Play page shows it under **Host a match**, with a **Copy** button.
4. Joiners put it in **Join a match** (or on the `join=` line of `server.ini`). The port is added automatically.

A joiner can check the path before launching anything: `ping 100.x.y.z` from a command prompt should answer.

## What sharing grants

Share the PC; don't invite joiners into your tailnet. An invited member can reach every device you have on Tailscale. Someone you share one PC with:
- **can** reach that one PC, on any port its firewall lets through;
- **can't** see or reach your other devices;
- **can't** see your internet traffic, which doesn't pass through their PC either (unless you set up an exit node and give them access to it).

Sharing goes one way. Tailscale's docs say shared machines are "quarantined by default": the shared PC can answer the people it's shared with but can't open connections to them. So joining doesn't open a joiner's PC to the host.

Accepting a share shows each side some details of the other. The host sees the joiner's email and avatar; the joiner sees the host's PC as owned by the host. When two PCs connect directly, each learns the other's public IP address, which is how they find each other. So Tailscale doesn't hide your home IP from the people you share with, only from everyone else. (See [Tailscale's sharing page](https://tailscale.com/kb/1084/sharing).)

On Windows, Tailscale's network may be classed as a **Private** network, where Windows Firewall lets more in than on a Public one: Network Discovery, for example. Without a rule like the one below, people you share with can reach anything the firewall allows on that network.

## Limiting shared players to the game

Tailscale's access rules can limit everyone you share with to the game server on the host's PC. In the admin console, open **Access controls**. If the policy file is still Tailscale's default (one rule letting everyone reach everything, plus an `ssh` block), replace it with this, putting your own Tailscale address in `hosts`:

```json
{
  "hosts": {
    "game-host": "100.x.y.z"
  },
  "acls": [
    // Your own devices: full access to each other, as before.
    {"action": "accept", "src": ["autogroup:member"], "dst": ["*:*"]},
    // People you've shared your PC with: only the game server.
    {"action": "accept", "src": ["autogroup:shared"], "proto": "udp", "dst": ["game-host:25200"]}
  ],
  "ssh": [
    {"action": "check", "src": ["autogroup:member"], "dst": ["autogroup:self"], "users": ["autogroup:nonroot", "root"]}
  ]
}
```

What it does:
- `autogroup:member` is everyone in your tailnet itself, usually just you. If you've invited other people as members, they keep full access too.
- `autogroup:shared` is everyone who accepted a share from you. They can reach UDP port 25200 on your PC and nothing else, on any machine you share.
- The default rule had to go, not just get a rule added beside it: Tailscale's docs say its `"src": ["*"]` includes shared users.
- With this older rule format, ICMP is allowed between two addresses whenever any traffic is, so the `ping` check above should still answer.
- The `ssh` block is Tailscale's default, kept as it was.

If your policy file already holds more than the default (groups, tags, other rules), add the `hosts` entry and the `autogroup:shared` rule to it, and narrow any rule whose source is `"*"` to `autogroup:member`.

Have one joiner test it before a match with others. If a join fails, **Reset to default** in the admin console brings back the allow-all policy. Don't put your filled-in file in a public place: it carries your Tailscale address.

The match itself is the one thing the rule can't close off: joiners talk to the game's own server code. It's an old game, and how carefully it handles bad data isn't known, so share with people you'd invite to a game anyway.
