# Flip 2 launcher (prototype)

A PocketJS launcher in place of EmulationStation on ROCKNIX (Retroid Pocket Flip 2). It lists Hollow Knight: Silksong (nx2linux) and RetroArch.

- `pocket-flip2`: the PocketJS program, built from the `flip2-launcher` branch of a PocketJS clone (`programs/flip2`, app `apps/flip2-launcher`). It draws on the DSI panel through DRM/KMS; the panel is mounted on its side, so the program turns each damaged rectangle onto it (`--rotate ccw`).
- `supervisor.sh`: runs the launcher until it prints `NX2L LAUNCH <id>`, ends it, runs the game in a sway session of its own (`sway-game.conf`) and starts the launcher again when the game exits.
- `launcherctl start|stop|restart|es`: starts the supervisor in place of EmulationStation and sway, or brings EmulationStation back. Nothing survives a reboot yet: ROCKNIX starts EmulationStation again.

Device folder: `/storage/nx2linux/launcher` (these files, `pocket-flip2`, `launcher.pocket`; log in `supervisor.log`).

Build (in the PocketJS clone):

```sh
bun tools/pocket.ts host export --config programs/flip2/pocket.host.config.ts --out build/host
POCKETJS_PROFILE_RS=build/host/pocket_profile.rs cargo zigbuild --release -p pocket-flip2 --target aarch64-unknown-linux-gnu.2.38
bun tools/pocket.ts compile --manifest apps/flip2-launcher/pocket.app.json --host-config programs/flip2/pocket.host.config.ts --output build/launcher.pocket
```

Measured at the menu, idle (10 s each):

| | Processes | RSS | CPU (one core) | System memory used |
| --- | --- | --- | --- | --- |
| EmulationStation + sway | 4 | 217 MB | 7.8% | 1125 MB |
| PocketJS launcher | 1 | 39 MB | 2% | 924 MB |
