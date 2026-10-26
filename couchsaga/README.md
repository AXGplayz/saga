# CouchSaga: online co-op for LEGO Star Wars: The Complete Saga

Two players, two houses, one shared game. Each PC runs its own copy of the game
(built from the [opensagadev/saga](https://github.com/opensagadev/saga)
decompilation), and the two copies stay in lockstep: every frame, each game
sends the other only its controller presses. Nothing else crosses the internet
(no video, no remote control, no file access beyond the host's save at start).

> **Status: experimental.** The netplay engine is tested (packet loss,
> reordering, disconnects, host-guest sessions over real sockets). Whether the
> *game itself* plays out identically on two PCs is what the **sync test**
> below checks. If it reports a difference, send the `netplay.log` and it can
> be fixed system by system.

## What you need

- Windows PC each, and a controller each (keyboard works too).
- The game data file `main.1060.com.wb.lego.tcs.obb` from your own copy of the
  Android release. CouchSaga never includes it.
- **The same CouchSaga download on both PCs.** Different builds are refused,
  because they would drift apart.

## Setup (both players)

1. Unzip CouchSaga anywhere.
2. Double-click **Start CouchSaga.bat**.
3. Click **Choose game data file** and pick your `.obb` (copied once into `res`).

## 1. Sync test (one PC, do this first)

1. Click **1. Record**, play for a few minutes (a story level, with fighting,
   building and a cutscene), then close the game.
2. Click **2. Replay** and keep your hands off. The game plays your recording
   back by itself. Watch the title bar:
   - *"Replay finished: all N checkpoints matched"* means the game is
     deterministic, so online co-op will stay in sync.
   - *"DIFFERS from the recording at frame N"* means something in the game
     still depends on timing. Send `res\netplay.log`.

## 2. Play online

**Host (Player 1, your save is used):**
1. On your router, forward port **7777** (TCP **and** UDP) to your PC. Or use a
   VPN like ZeroTier, which needs no port forwarding.
2. Click **Host game**. When Windows Firewall asks, allow it on private networks.
3. Click **Find my internet address** (copies `address:7777`) and send it to
   your friend.

**Friend (Player 2):**
1. Paste the address into **Host address** and click **Join game**.
2. The game boots from a copy of the host's save. Your own saves are untouched.

Both games start together. The title bar shows ping, input delay and whether
the two games are **in sync**. Player 2 takes over the second character, like a
second controller on a console.

## Controls

| Action | Controller (Xbox layout) | Keyboard |
|---|---|---|
| Move | Left stick / D-pad | WASD / arrows |
| Jump | A | Space |
| Attack / use | X | E |
| Special | B | F |
| Swap character | Y | K |
| Pause | Start | Enter / Esc |

## If something goes wrong

- **"different versions" / "game data files are different":** use the same
  CouchSaga zip and the same `.obb` on both PCs.
- **Friend can't connect:** port 7777 TCP isn't forwarded, or the host isn't on
  the *Host game* screen yet.
- **"game traffic (UDP) is blocked":** forward UDP 7777 as well as TCP.
- **Laggy:** the input delay is picked from your ping automatically. Wired
  connections help most.
- **"OUT OF SYNC":** the two games drifted apart. Restart both and send both
  `res\netplay.log` files; they show which part of the game diverged.
- **Short pauses when entering new areas:** expected. Online co-op loads areas
  in the foreground so both PCs load them on the same frame.

## What gets opened to the internet

Only while you're hosting: one port, to this game, which accepts a single
friend running the same build. Joining opens nothing; the friend's PC only
dials out. Close the game and the port is no longer answered.

## Command line

`saga_native.exe window --host [--port 7777] [--delay N]`,
`--join ADDRESS[:PORT]`, `--two-player`, `--record FILE`, `--replay FILE`.
Run from the folder that contains `res\`.
