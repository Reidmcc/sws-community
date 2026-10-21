# Squadrons socket manager / socket interface (static analysis, 2026-10-20)

Source: `work/starwarssquadrons.exe` (no ASLR, base 0x140000000). Every claim is tagged
**[V]** = VERIFIED (code read: Ghidra decompile and/or capstone disassembly) or **[I]** = INFERRED.
The only implementation in the exe is an in-memory loopback ("memory socket"); "in-mem" below means that implementation.
ABI: MSVC x64 `__thiscall` = Microsoft x64: `this`=RCX, args RDX, R8, R9, then stack; floats in XMM by position
(arg 2 in XMM1, arg 3 in XMM2); struct-by-value return via hidden pointer in RDX (after `this`), returned in RAX.

Address helpers [V]: `FUN_140296090(const char* "host:port", SocketAddr* out)` = strchr(':') (no ':' -> fail),
host = text before the first ':', port = atoi(after) -> htons. `FUN_1402961e0(host, port, out)`: "" -> INADDR_ANY;
"localhost" -> gethostname + getaddrinfo(AF_INET), first address (NOT 127.0.0.1), fallback 127.0.0.1; else
inet_pton(AF_INET) then getaddrinfo(AF_INET). Output: 16-byte sockaddr_in, length byte 0x10 at +0x40. IPv4 only.
ws2_32 ordinals used: 8 htonl, 9 htons, 14 ntohl, 15 ntohs, 57 gethostname. Lock = EnterCriticalSection on DAT_1441ef030.

## 0. Data types [V]
- `SocketAddr` = `{ uint8 data[64]; uint8 len; }` (65 bytes, align 1). IPv4 = sockaddr_in in data[0..15], len 0x10.
- `PacketInfo` = `{ int minSize, maxSize, recommendedSize, overheadWhenAligned, alignment; }` (20 bytes).

## 1. Socket manager

Object (in-mem, 0x30 bytes, built inline in FUN_140b9eaa0 and FUN_140cb0080) [V]:
`+0x00` vtable 0x1433fa218 (ISocketManager), `+0x08` vtable 0x1433fa210 (SocketCreator base),
`+0x10/+0x18` intrusive list head (next, prev; self-linked when empty) of nodes `{next, prev, Socket*}` (0x18 bytes),
`+0x20` u32 node count, `+0x28` arena pointer (lazily created if 0).
ISocketManager base vtable 0x1433fa1e8 = 4 x _purecall + base deleting dtor 0x140295b20 (size 8) [V]; so the
interface is 4 pure methods + a virtual destructor, i.e. exactly Kyber's `SocketManager : ISocketManager, SocketCreator`
with `virtual ~SocketManager()` landing in slot 4.

| slot | addr | name | signature (engine view) | in-mem behavior |
|---|---|---|---|---|
| 0 (+0x00) | 0x140295bb0 | Destroy | `void Destroy()` | `if(this) this->vtbl[4](this,1)` [V] |
| 1 (+0x08) | 0x140295dd0 | Connect | `ISocket* Connect(const char* hostPort, bool blocking)` | see below [V] |
| 2 (+0x10) | 0x140295c40 | Listen | `ISocket* Listen(const char* hostPort, bool blocking)` | see below [V] |
| 3 (+0x18) | 0x140295f90 | CreateSocket | `ISocket* CreateSocket()` | see below [V] |
| 4 (+0x20) | 0x140295b50 | ~SocketManager (scalar deleting dtor) | `void* dtor(unsigned flags)` | frees list NODES only (FUN_1401c16a0), NOT the sockets; resets vtable to 0x1433fa1e8; if flags&1 frees 0x30 bytes [V] |
| 2nd base slot 0 (vtable 0x1433fa210) | 0x140295bd0 | SocketCreator::Close | `void Close(Socket*)`, `this` = manager+8 | unlinks and frees every list node whose socket == arg; does not free the socket [V] |

**Correction to the earlier notes:** manager slot 4 is the deleting destructor, NOT Close. Close lives only in the
second base [V]. The engine never calls Close itself; only the in-mem socket dtor calls it (`[sock+8]->vtbl[0](creator, sock)`, FUN_140295050) [V].

Connect (0x140295dd0) [V]: if `*hostPort == 0` return null. Allocates 0xe0 from manager arena (FUN_1401de690 align 0x10),
constructs with FUN_140294f10(mem, creator = this+8). Parses hostPort; on success writes the PEER address
(+0x58, len +0x98), appends a node to the manager list, returns socket. On parse failure deletes the socket (vtbl[0](s,1)) and returns null.
`blocking` (R8) is never read (first use of R8 is a write) [V].
Listen (0x140295c40) [V]: same allocation/ctor; parses hostPort into the LOCAL address (+0x10, len +0x50), port =
ntohs(sin_port) if len==0x10 else 0, stored at +0x54; appends to list; returns socket, or deletes it and returns null on
parse failure. Empty host is allowed (INADDR_ANY). Does not call socket slot 18; it inlines the same work. `blocking` ignored [V].
CreateSocket (0x140295f90) [V]: allocate+construct, peer = AF_INET zero address (len 0x10), append, return.
Ownership [V]: the caller owns the returned socket and deletes it with `sock->vtbl[0](sock, 1)` (FUN_140d75fc0 at
0x140d7612c). Destroy does not delete outstanding sockets; a socket outliving its manager would call Close on freed memory
(in-mem hazard) [V].

Engine call sites:
- Listen [V]: FUN_140d76190(peer, mgr, hostPort, a4, a5): `peer+0x4518 = mgr; s = mgr->Listen(hostPort, false)`
  (R8 = 0); null -> return false; `peer+0x4510 = s`; FUN_140d7bb70(transport = peer+0xf0, s) sets `transport+0x740 = s`,
  receive buffer size `transport+0x870 = 0x640` and a 0x640-byte buffer at `+0x868`.
  - Server: FUN_140b9eaa0 builds "`<server+0xf0>:<server+0xf8>`" (address default "" or "127.0.0.1" when
    spawnInfo+0x25 / localhost), calls FUN_140d76190(server+0x98, mgr, ...); on failure Sleep(100) or port+1 (settings+0xa8),
    up to server+0xfc attempts. Server marks `transport+0x758 = 1`, `+0x759 = 1`.
  - Client: FUN_140cb0890 ALSO uses Listen (not Connect): "`<addr>:<port>`" with addr = "" or "127.0.0.1"
    (connobj+0x31 localhost), port starting at settings+0x30, up to 64 tries with port+1. Then FUN_140cb05c0 sets the
    server address with socket slot 5 (SetPeerAddress) at 0x140cb0657.
- Destroy [V]: server shutdown FUN_140b9e570 at 0x140b9e921: `if (server+0x110) server+0x110->Destroy(); server+0x110 = 0`.
  server+0x110 is only set when the server created the in-mem manager itself; an externally supplied manager
  (FUN_140b9eaa0 param 4, or overrides+8) is never destroyed by the server [V]. No client-side Destroy of connobj+0x38 found [I: not located].
- Connect / CreateSocket: no engine call site found through connobj+0x38, peer+0x4518 or server+0x110 [V for those
  paths; other users not excluded].
- Manager acquisition [V]: server FUN_140b9eaa0(server, info, overrides, mgr): mgr ?: overrides+8 ?: new in-mem (server+0x110).
  Client FUN_140cb0080(connobj, mode, creator, ., .): if connobj+0x38 already set keep it; mode 0/2 -> new in-mem;
  mode 1 -> `creator->vtbl[0](creator)` (or creator[4] if vtbl[0]==FUN_140200ae0); result at connobj+0x38.

## 2. Socket

Layout (in-mem class, 0xe0 bytes; ctor FUN_140294f10, dtor body FUN_140295050) [V unless noted]:
| off | type | meaning |
|---|---|---|
| +0x00 | vtable | 0x1433fa240 |
| +0x08 | SocketCreator* | creator = manager+8 (Close target in dtor) |
| +0x10 | SocketAddr | local address (Listen); len at +0x50 |
| +0x54 | int | local port, host order (Port()) |
| +0x58 | SocketAddr | peer address (Connect/SetPeerAddress/after receive); len at +0x98 |
| +0x99 | SocketAddr? | zeroed 65 bytes (0x99..0xd9), unused by in-mem [I: broadcast address, as in Kyber] |
| +0xda | bool | if set, slot 7 reports max/recommended 0x63e instead of 0x640; never set by in-mem [V], meaning unknown |
Ctor/dtor also inc/dec a global live-socket count DAT_14417d7b4; when it hits 0 the dtor frees every queue.

ISocket base vtable 0x1433fa2e8 has 17 slots: [0] base dtor 0x1402933b0, [1] _purecall, [2] 0x1402933a0 (non-pure
default), [3..16] _purecall [V]. So ISocket has 17 virtuals (Kyber: 16) and IUDPSocket adds 3 (slots 17-19).

| slot | addr | name (ours) | signature (engine view) | in-mem behavior / fields |
|---|---|---|---|---|
| 0 (+0x00) | 0x140294ff0 | ~Socket (scalar deleting) | `void* dtor(unsigned flags)` | creator->Close(this); drop this port's queue (or all queues if last socket); free 0xe0 if flags&1 [V] |
| 1 (+0x08) | 0x1402951c0 | Send | `bool Send(const uint8* buf, int size, unsigned flags)` | sends to PEER (+0x58). Packet buffer 0x6cc from global pool: src addr = local (+0x10/+0x50) at [0..0x40], dest = peer at [0x41..0x81], data at +0x82 (no size check; room 1602), size at +0x6c4, age float +0x6c8 = 0. Appended to the queue keyed by ntohs(dest port) (0 if dest len != 0x10). flags ignored. Returns 1 [V] |
| 2 (+0x10) | 0x1402933a0 | ReceiveFromWhen (base default) | `int (uint8* buf, int maxSize, uint32& receivedWhen)` | inherited base impl: `receivedWhen = 0; return this->slot3(buf, maxSize, receivedWhen)` (tail jump, args unchanged) [V]. **This is the slot the engine calls.** |
| 3 (+0x18) | 0x140295480 | ReceiveFrom (pure in base) | `int (uint8* buf, int maxSize[, uint32& when])` | look up queue for own port (+0x54); first packet whose dest addr == local addr (len+bytes), or IPv4 dest with port == own port and IP 255.255.255.255; copy data to buf (maxSize NOT checked), set peer (+0x58/+0x98) = packet source, recycle buffer, return size. None -> **-1**. R8/R9 unused [V] |
| 4 (+0x20) | 0x1401aa060 | SetBroadcast | `bool (uint16 port)` | `return false` [V] |
| 5 (+0x28) | 0x140295800 | SetPeerAddress | `void (const SocketAddr&)` | copy len (+0x40) bytes to +0x58, len to +0x98 [V] |
| 6 (+0x30) | 0x140295830 | PeerAddress | `SocketAddr PeerAddress() const` (hidden ret ptr in RDX) | zero 65-byte out, copy +0x58/+0x98, return out [V] |
| 7 (+0x38) | 0x140295890 | GetPacketInfo (Kyber: SetDefaultPacketInfo) | `bool (PacketInfo* out)` | out = {1, 1600, 1600, 0, 1} (1598 if +0xda); return true [V] |
| 8 (+0x40) | 0x1401aa060 | BlockingMode | `bool () const` | `return false` [V] |
| 9 (+0x48) | 0x1401ae7c0 | SetBlockingMode | `bool (bool)` | `return true` [V] |
| 10 (+0x50) | 0x1401a8250 | ReceivePulse | `void ()` | no-op [V] |
| 11 (+0x58) | 0x1402958e0 | Pulse | `void (float seconds)` (XMM1) | ages EVERY queued packet in EVERY queue by `seconds`; drops packets older than 10.0 (DAT_1437ce49c) [V] |
| 12 (+0x60) | 0x1401a8250 | SendPulse | `void ()` | no-op [V] |
| 13 (+0x68) | 0x1401a9760 | NativeSocket | `intptr_t () const` | `return 0` [V] |
| 14 (+0x70) | 0x140295b00 | Port | `int () const` | `return +0x54` [V] |
| 15 (+0x78) | 0x1401b9bc0 | Address | `const SocketAddr* () const` | `return this+0x10` [V] |
| 16 (+0x80) | 0x1401a8250 | **extra (unknown)** | `void (?)` | no-op; pure in the ISocket base [V]. No engine caller found [V for transport paths]. Not in Kyber. |
| 17 (+0x88) | 0x140295700 | Connect | `bool (const SocketAddr&, bool blocking)` | peer = AF_INET zero, then copy arg into peer; return true [V] |
| 18 (+0x90) | 0x1402957a0 | Listen | `bool (const SocketAddr&, bool blocking)` | local = arg (+0x10/+0x50); port = ntohs if len==0x10 else 0 -> +0x54; return true [V] |
| 19 (+0x98) | 0x140295760 | Create | `bool (bool blocking)` | peer = AF_INET zero (len 0x10); return true [V] |

Engine call sites (socket = transport+0x740 = peer+0x4510) [V]:
- slot 0: FUN_140d75fc0 (peer shutdown) `s->vtbl[0](s,1)`, then peer+0x4510 = 0.
- slot 1: FUN_140d78830 (per-connection send, called by FUN_140d7d790): under lock `s->SetPeerAddress(connAddr); return s->Send(buf, size, 0)`. Also FUN_140d78c00 (connectionless message: 4x {Send(buf,len,0); ReceivePulse(); Pulse(3.0f); SendPulse()} then PeerAddress) [I: purpose].
- slot 2: FUN_140d7b690 at 0x140d7b6e8: `when = 0; n = s->slot2(transport+0x868, transport+0x870 /*0x640*/, &when)`.
- slot 3: no direct engine call found; reached only via slot 2's base default.
- slot 5: FUN_140d78830 (before every Send); FUN_140cb05c0 at 0x140cb0657 (client: set server address).
- slot 6: FUN_140d7a9a0 (packet dispatch after receive: PeerAddress() -> FUN_140baa1a0 address compare to pick the connection; unknown sender -> handshake path / "UNKNOWN_ID" callback); FUN_140d78c00.
- slot 7: FUN_140d76240 at 0x140d762c6 (client connect) and FUN_140d72a50: if it returns true the engine uses
  overhead += 0x18, max -= 0x18, alignment = (alignment+7)&~7, recommended = min(recommended, max-0x18) for the connection.
- slots 10/11/12: see section 3.
- slots 4, 8, 9, 13-19: no engine call found via transport+0x740 / peer+0x4510 [V for those paths].

## 3. Per-frame driving [V]
One socket per transport: the server has ONE listening socket for all clients; the client has one socket (listened on a
local port) whose peer is set to the server. Outgoing: SetPeerAddress(dest) + Send(buf, size, 0) per packet. Incoming:
ReceiveFromWhen then PeerAddress() identifies the sender.
- Receive: FUN_140d76d40(peer): `s->ReceivePulse()`, then FUN_140d745d0 loops `FUN_140d7b690` (slot 2) until it returns
  false. **Return contract:** n > 0 = packet; n < 0 = queue empty (loop ends); **n == 0 makes FUN_140d7b690 return true and
  the loop spins forever** (setns at 0x140d7b710). Packets are routed by connection id to FUN_140d73a60(conn, bitstream, when).
  Server: FUN_140b9f330 -> FUN_140b9fcb0 -> FUN_140d76d40(server+0x98). Client: connobj vtable slot 17 FUN_140cb0350 -> FUN_140d76d40.
- Pulse: FUN_140d76670(peer, ?, dt in XMM2) -> FUN_140d7cb30(transport, dt in XMM1) -> `s->Pulse(dt)` first thing.
  Server: FUN_140bafa70(this, ., dt=arg3 float) ; client: connobj slot 18 FUN_140cb0440. dt is the frame delta in seconds [I: units].
- Send: FUN_140d768d0(peer) flushes connections (Send via FUN_140d78830) and ends with `s->SendPulse()` (0x140d76c48).
  Server: FUN_140b9f330 calls receive (FUN_140b9fcb0) then send (FUN_140d768d0). Client: FUN_140cb04d0 -> FUN_140d768d0.
- Client-only latency simulator FUN_140d7b2d0 (active when DAT_1441e297c and transport+0x759 == 0) can supply packets
  before the socket is read; the server disables it via +0x759 = 1.

## 4. Time base of receivedWhen [V]
Unit = DirtySDK NetTick milliseconds (uint32): the function pointer at **0x143e2c2b0** (default 0x140696ce0 = jmp
timeGetTime; FUN_140696cf0 (NetLib init) switches it to 0x140696cb0 = QPC*1000/freq when DAT_14417fd4c != 0).
Consumer: FUN_140d73a60 stores `when` at record+0x10 (0x90-byte ring); FUN_140d9b540 reads it -> FUN_140d9b6a0 ->
FUN_140d9b410: `if (when != 0) latency -= (NetTick() - when) * 0.001f` (clamped >= 0). So 0 = "no timestamp", safe but
loses this compensation. In-mem never sets it (always 0). Kyber's BF2 NetTick (0x140754D60) is the same DirtySDK clock [I].
To match exactly, call `(*(uint32(**)())0x143e2c2b0)()` when the datagram is received.

## 5. Differences from Kyber's BF2 interface
1. Manager: same 5-slot primary vtable (Destroy, Connect, Listen, CreateSocket, virtual dtor) + SocketCreator::Close in the
   second base [V]. Earlier note "slot 4 = Close" was wrong.
2. Socket has 20 slots: an extra pure virtual at slot 16 (after Address, before IUDPSocket::Connect); Kyber's Connect/Listen/
   Create shift from 16/17/18 to 17/18/19 [V]. Slots 0-15 keep Kyber's order [V for the in-mem bodies].
3. Slots 2/3: the base slot 2 takes `uint32& when` (writes [R9]) and forwards to slot 3; the engine calls slot 2 with
   (buf, 1600, &when) [V]. Kyber's slot 2 is `ReceiveFrom(buf, size)` (ignores R9) and its slot 3 sets `when`; in Squadrons
   that combination would never deliver `when` (engine never calls slot 3). Override slot 2 directly.
4. Slot 7 is a getter: fills PacketInfo and returns bool [V] (Kyber names it SetDefaultPacketInfo).
5. The client uses manager Listen (local "" or "127.0.0.1", port from settings+0x30, 64 tries) + SetPeerAddress, not manager
   Connect [V]. `blocking` args are always false/ignored [V].
6. Max datagram the engine accepts is 0x640 = 1600 bytes (receive buffer transport+0x868, size +0x870) [V].

## 6. Implementation checklist (C++ replacement)
- Manager class, primary base vtable in this order: `Destroy()`, `Connect(const char*, bool)`, `Listen(const char*, bool)`,
  `CreateSocket()`, `virtual ~Mgr()`. Return our socket pointer cast to the ISocket base (must be the object start, vtable at +0).
  Parse "host:port" yourself with the same rules as FUN_140296090 (first ':', "" = any, "localhost" = own address).
  A second base for Close is optional (engine never calls it); unregister sockets from your own dtor instead.
- Socket class: one vtable of exactly 20 entries, order: 0 `virtual ~Socket()` (MSVC emits the scalar deleting dtor,
  engine calls `vtbl[0](s, 1)` and expects the memory freed); 1 `bool Send(const uint8_t*, int, unsigned)`;
  2 `int ReceiveFromWhen(uint8_t*, int maxSize, uint32_t& when)` (do the real work here); 3 same signature (alias);
  4 `bool SetBroadcast(uint16_t)`; 5 `void SetPeerAddress(const SocketAddr&)`; 6 `SocketAddr PeerAddress() const`;
  7 `bool GetPacketInfo(PacketInfo*)`; 8 `bool BlockingMode() const`; 9 `bool SetBlockingMode(bool)`; 10 `void ReceivePulse()`;
  11 `void Pulse(float)`; 12 `void SendPulse()`; 13 `intptr_t NativeSocket() const`; 14 `int Port() const`;
  15 `const SocketAddr* Address() const`; 16 `void Unknown16()` (no-op); 17 `bool Connect(const SocketAddr&, bool)`;
  18 `bool Listen(const SocketAddr&, bool)`; 19 `bool Create(bool)`. Use plain virtuals in one class with no other bases
  (or a static_assert on the vtable layout) so MSVC keeps this order. Engine reads no data fields directly.
- Must be real: 0, 1 (send to current peer; may be called from more than one thread, engine holds its own lock in FUN_140d78830),
  2 (non-blocking; return -1 when empty, never 0; clamp to maxSize <= 1600; set peer = sender; set `when` = NetTick or 0),
  5, 6 (after each receive returns the sender), 7 (max <= 1600 incl. our own framing overhead), 14, 15, 18 (bind; called
  indirectly through our manager Listen).
- Can be trivial: 4 (false), 8 (false), 9 (true), 10/11/12 (no-op, or 10 = drain OS socket into a queue, 12 = flush),
  13 (SOCKET handle or 0), 16 (no-op), 17/19 (store peer / return true), 3 (forward to 2).
- Hook points: server FUN_140b9eaa0 param 4 (manager) and client FUN_140cb0080 mode 1 creator (see section 1).

## Open / not determined
- Purpose and signature of socket slot 16 (no caller found).
- Meaning of socket +0xda (shrinks max packet by 2 bytes) and the third SocketAddr at +0x99.
- Whether anything outside the transport paths above calls manager Connect/CreateSocket or socket slots 3/4/8/9/13-19.
- Client-side manager Destroy site (connobj+0x38) not located.
