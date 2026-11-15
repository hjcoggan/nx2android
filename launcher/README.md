# Flip 2 launcher (prototype)

A PocketJS launcher in place of EmulationStation on ROCKNIX (Retroid Pocket Flip 2). It lists Hollow Knight: Silksong and Hollow Knight (nx2linux, through `/storage/nx2linux/launch.sh` with `DATA=/storage/nx2linux/<game>`) and RetroArch.

- `pocket-flip2`: the PocketJS program, built from the `flip2-launcher` branch of a PocketJS clone (`programs/flip2`, app `apps/flip2-launcher`). It draws on the DSI panel through DRM/KMS; the panel is mounted on its side, so the program turns each damaged rectangle onto it (`--rotate ccw`).
- `supervisor.sh`: runs the launcher until it prints `NX2L LAUNCH <id>`, ends it, runs the game in a sway session of its own (`sway-game.conf`) and starts the launcher again when the game exits.
- `nx2l-launcher.service`: the supervisor as a systemd unit; it conflicts with `sway.service` and `essway.service`.
- `launcherctl enable|disable|start|stop|es`: `enable` copies the unit to `/storage/.config/system.d` and writes `/storage/.config/profile.d/099-nx2l`, which sets ROCKNIX's `UI_SERVICE` to it, so `/usr/bin/autostart` starts the launcher at boot instead of sway and EmulationStation; `disable` removes both. `es` starts EmulationStation now (stopping the launcher).

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
