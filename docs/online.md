# Online play: how NFS II SE's netcode behaves

NFS II SE's network mode is EA's IPX layer, the same one NFS3 uses: packets start with
`EA dat`, and the race is a **host-relayed input lockstep at about 32 Hz**.

- Each client sends one small packet per game tick (16 bytes: header, two counters, a checksum
  and its 6-byte controller state).
- The host gathers every player's input for the tick and sends the bundle back (43 bytes for two
  players).
- Every copy of the game simulates all cars from the same inputs, so nothing about car positions is
  sent at all. The host's own car reacts at once; a client's input comes back to it with the bundle,
  so **a client feels one round trip to the host as input lag**. That's why the lobby recommends the
  player with the lowest worst-case ping as host.

## Measurements (native, two headless instances, `tools/nettest.sh`)

Delay is added to incoming packets on both sides (`RT_NET_DELAY`), so a round trip gets twice it;
loss drops incoming packets (`RT_NET_LOSS`). Numbers are for 25 s of racing.

| Link | Host fps | Client fps | Packets/s each way | Notes |
|---|---|---|---|---|
| direct | 60 | 60 | 32 | |
| +50 ms (100 ms round trip) | 60 | 59.7 avg | 32–39 | |
| +120 ms (240 ms round trip) | 60 | 60 | 32–33 | both copies stay in step (same race clock, same places on track) |
| 3 % loss | 60 | 57.5 avg, 52 min | 33–34 | a lost bundle holds the client until EA's own resend |

The game doesn't freeze the picture while it waits: frames keep coming at 60 Hz, so even a
quarter-second round trip shows up as input lag on the client, not as stutter.

In the browser, lost packets matter less than in the native test above: the WebRTC transport
(`web/net.js`) sends every packet with the previous ones attached, so a lost one is recovered with
the next packet (about 31 ms later) instead of waiting for EA's resend.

With a long round trip the lobby is slower too: joining and the game list take a round trip or
two before they update. Give it a moment.

## Smoother play (from NFS3 Web)

The runtime trades strict lockstep for responsiveness, as NFS3 Web does (`runtime/net.c`;
`RT_NETSYNC=0` turns it off):

1. A client writes its newest controls into its own block of each bundle it receives (and fixes
   the CRC), so its car reacts in its own game without waiting for the host. It learns which block
   is its own from the first controls it sends.
2. Every player's game is the authority for its own car, the host's for the AI cars: about 30
   times a second each sends those cars' physics state (88 bytes of 16.16 fixed point from car
   +0x9c: position, velocity, orientation, spin) and the others overwrite theirs, blending small
   corrections. The host moves a client's car ahead by half the round trip.

NFS4's own answer to the same problem is its *Positional Updates* mode: each machine drives its
own car and sends its position. NFS II SE has only lockstep, so smoother play gives it the same
model from the runtime.

What it needed from `NFS2SEA.EXE`: the car table at `0x50f918` (records 0x690 apart, car k =
player k on every machine, then the AI), the physics block at +0x9c (found by snapshotting guest
memory during a race and matching positions to velocities), message type 0x05 (NFS3: 0x06), and
the CRC (same EA CRC-16, computed here instead of read from the game's table).

Measured at a 120 ms round trip, from a standstill (`RT_CARLOG=50`, two native instances):

| | Client's car starts moving in its own game | Paths agree (95%) |
|---|---|---|
| plain lockstep | 349 ms after the key | 0.09 units |
| smoother play | 258 ms after the key | 0.15 units |
| for comparison: the host's own car | 214–237 ms after the key | |

About 220 ms of that is the game itself (its input scheduling and getting the car rolling), the
same for everyone; smoother play removes the round trip a client used to add on top, so every
player's car now responds like the host's.

## Player names

The page passes the lobby name to the game (`RT_PLAYER_NAME`, `runtime/game.c`), which writes it
into the game's own network name once its settings are loaded: the settings block at `0x510b70`
(saved as `FEDATA/PC/CONFIG/config.dat`), name at +0x1ac, 8 characters. So CONNECT's "enter
name" already shows the lobby name, on a first visit too. The field was found by saving two races
in which only the typed name differed (the game saves its settings when a race starts).

## What could be better

- **More players.** Tested with two; three to eight should follow the same rules (car k = player k).
