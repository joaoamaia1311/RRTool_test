# RRTool source study guide

RRTool is a Windows C/C++ external overlay for ROB RUN. This source snapshot
is a teaching example of memory transport, object discovery, concurrent
snapshots, 3D projection, skeletal animation and a native/HTML settings bridge.
It also contains privileged and state-changing operations. Read their code
before using them; the offline exercises below need no running game or driver.

This folder has two Markdown guides: **README.md** explains the project and
**[BUILD.md](BUILD.md)** explains builds, validation and offset maintenance.
The source headers explain local contracts. JSON files in `offsets` retain
field and rig provenance. Historical session logs and separate research notes
are not part of this teaching edition.

## Folder map

| Path | Purpose |
|---|---|
| `1_run/` | Prebuilt artifacts, settings and launch scripts |
| `2_src_port/` | Overlay, reader, renderer, menu and build script |
| `3_src_driver/` | SmxRing kernel driver and its build script |
| `4_src_mapper/kd2_src/` | Mapper solution and source |
| `offsets/` | Managed field definitions and rig provenance in JSON |
| `tools/` | Offset generation and auditing scripts |

The overlay build produces `RobRunPort.exe`. Some identifiers and log strings
retain older names such as `RobRunESP` or `RobRunAIO`; they do not identify
additional build targets in this folder. Included binaries are artifacts from
earlier builds. Rebuilding is how to validate the source you are studying.

## Suggested reading order

1. Read [rr_game.h](2_src_port/rr_game.h) to understand `RR_PLAYER`, `RR_CAMERA`
   and the snapshot API. Addresses are `uint64_t` values in another process;
   they cannot be dereferenced as ordinary local pointers.
2. Read [rr_unity.h](2_src_port/rr_unity.h) and
   [rr_unity.c](2_src_port/rr_unity.c) for object, string, array and dictionary
   access. Separate a valid zero value from a failed memory read.
3. Follow [rr_mem.h](2_src_port/rr_mem.h) into
   [rr_mem.c](2_src_port/rr_mem.c). Study request layouts, reply limits,
   return values and the thread-local small-read status.
4. Follow `game_init()`, `scan_worker()` and `game_snapshot()` in
   [rr_game.c](2_src_port/rr_game.c). Note where identities are checked and
   where a lock protects publication rather than the whole scan.
5. Follow `game_w2s()` into [rr_draw.c](2_src_port/rr_draw.c) and
   [gpu.cpp](2_src_port/gpu.cpp). Track world coordinates through camera space
   to screen pixels, then through drawing and presentation.
6. Read [menu_bridge.h](2_src_port/menu_bridge.h),
   [menu_webview.cpp](2_src_port/menu_webview.cpp) and
   [menu.html](2_src_port/menu.html) together. Trace a slider from a page message
   to a validated setting and back to serialized state.
7. Read `WinMain`, `ReadThread`, `RenderThread` and `OverlayProc` in
   [rr_esp.c](2_src_port/rr_esp.c) to see how the pieces are scheduled.
8. Study [smx_drv.c](3_src_driver/smx_drv.c) with its wire contract in
   [smx.h](3_src_driver/smx.h), then the mapper, after understanding the
   user-mode contracts. Their source is separate from the offline lessons.

## How one frame works

The memory transport sends an IOCTL (I/O control request) to SmxRing. The
driver copies requested target bytes into its buffered reply. User-mode
helpers interpret those bytes as the supported IL2CPP layouts and validate
class names, counts, positions and object links.

A discovery worker refreshes the world, roster and slower marker caches.
The sampling thread copies published cache entries under a lock, reads current
transforms and rigs, and publishes a caller-owned snapshot. The renderer uses
that snapshot for labels, radar, skeletons and reference meshes. Camera motion
is sampled separately near painting to reduce visible delay.

The renderer uses Direct2D/DirectWrite for 2D shapes and text. Character meshes
are rasterized on the CPU and uploaded as textures. D3D11 and DirectComposition
present the transparent overlay surface. Presentation can be skipped when the
frame signature has not changed or the compositor queue is full.

These reads are not an atomic snapshot of Unity. The game can change between
requests. Identity rechecks, capacity bounds, recent-pose retention and retries
reduce inconsistent results; they cannot make another process's writes atomic.

## Source map

| Files | Concepts to study |
|---|---|
| `rr_esp.c` | Win32 lifecycle, input delivery, settings, workers, diagnostics |
| `rr_mem.*` | Buffered transport, bounded batches, process/module lookup |
| `rr_unity.*` | Managed-object layouts and pointer-chain helpers |
| `rr_chain.*` | Parsing and evaluating optional named address expressions |
| `rr_game.*` | Discovery, validation, cache publication, pose sampling |
| `rr_draw.*`, `gpu.*` | Projection, filtering, rasterization and composition |
| `rr_aim.*` | Region selection, screen error and calibrated steering math |
| `rr_los*` | Capturing geometry and three-state line-of-sight queries |
| `rr_pose_gate.h`, `rr_cham_coherence.h` | Bounded confirmation of abrupt poses |
| `rr_rig_map.h`, `rr_body_joints.h` | Rig ancestry and anatomical link selection |
| `rr_hiding.h` | Validated hiding-object evidence and bounded dictionary walks |
| `rr_world_marker_select.h` | Keeping a bounded set of nearby markers |
| `rr_find.*`, `rr_classscan.c`, `rr_nodescan.c` | Bounded graph and memory searches |
| `rr_spawn.*` | Process creation, handle ownership and optional helper loading |
| `menu_bridge.h`, `menu_webview.cpp`, `wv2_cb*.h` | Settings schema and COM callbacks |
| `robrun*_offsets.h`, `rr_cham_model.h`, `rr_cham_types.h` | Generated field and rig data |

Bundled WebView2 headers and mapper dependencies include upstream code and
license notices. Preserve their attribution. Generated field names, hashes,
bone names and palette indices are data; keep them aligned with their inputs.

## Memory and validation lessons

IL2CPP is Unity's managed-code-to-native-code runtime. In the layout used here,
an object starts with a class pointer and monitor, followed by instance fields.
Strings store UTF-16 characters; arrays carry a length and an element area.
Dictionary entries have a stride that depends on their key/value types.

An **offset** locates a field relative to an object. An **RVA** is an address
relative to a loaded module. A heap **instance address** belongs to one process
and can become stale during a scene change. Keep these three meanings distinct.

The generated headers describe managed fields from a particular dump. They do
not validate native Unity structures, generic payload layouts, method bodies
or current heap identities. Class names and finite coordinates are plausibility
checks. A failed read means unknown; it does not prove an empty collection,
an absent player or a clear line of sight.

`rr_read()` limits one request to 64 KiB. `rr_read_batch()` accepts up to 64
items and splits payloads above its `0xF000` budget. The driver reply starts
with data at buffer offset zero, followed by per-item status bytes; it replaces
the request header. The client copies successful items and returns their count.
A positive count does not mean that every requested item was copied.

Line of sight uses three results: `CLEAR`, `BLOCKED` and `UNKNOWN`. Stale,
unsupported or inconsistent geometry remains unknown. This is a useful pattern
for any system where missing evidence must not become a positive result.

## Rig and pose lessons

The included character palette has 94 slots. The reference body/head mesh has
86 weighted bones; eight slots are optional for skinning. Anatomical display
excludes the weapon, magazine and three bag helpers, leaving up to 89 body
joints and 88 links. Missing optional joints suppress the affected links.

Native joints are matched by name and ancestry. The reader builds world poses
and skin matrices, then the renderer filters small motion relative to the root.
Abrupt branch changes are confirmed over a bounded interval in `rr_pose_gate.h`.
This reduces brief alternate poses while adding a short delay to abrupt motion.
The shared reference mesh can differ from the character's clothing or equipment.
The original asset exporter is not included; provenance remains in
`offsets/robrun_rig.json` and the generated headers.

## Offline exercises

Build the overlay using BUILD.md, then run these commands from `2_src_port`:

```powershell
.\RobRunPort.exe --selftest --nohide --no-pause
.\RobRunPort.exe --menu --nohide
.\RobRunPort.exe --demo --nohide
```

The manifest requests administrator elevation for every mode. `--nohide`
prevents the startup visibility request; it is explicit here because menu/demo
modes share the normal startup path. The exercises need neither a game nor a
mapped driver. Close menu mode with its window controls; use End to quit demo.

Suggested lessons:

1. Inspect `selftest.log` and match each check to `run_selftest()`. Add a new
   projection case on paper, then calculate its expected screen coordinates.
2. Run demo mode and trace one synthetic player through projection and drawing.
   Explain why the same world movement occupies fewer pixels at greater depth.
3. Run menu mode, change a setting, save, and inspect `CoreRR.cfg`. Trace the
   setting name through `BR_SETTINGS`, the HTML control and message handling.
4. Follow a failed batch read through its return paths. Identify which buffers
   remain unchanged and how a caller decides whether to retry individually.
5. Read the pose gate and describe what happens when a candidate pose returns
   to the accepted pose before the confirmation interval ends.

## Controls and settings

| Input | Action |
|---|---|
| F1 | Toggle the settings menu |
| Insert | Toggle the overlay |
| End | Quit |
| Configured aim key | Hold for aim assistance when enabled |

Several input paths are combined, including Ctrl-modified registered hotkeys
and plain-key hook/polling paths. The log records which path delivered an action.
Normal operation watches for game exit; offline menu/demo modes do not need it.

Settings are stored beside the executable in `CoreRR.cfg`. The schema in
`menu_bridge.h` defines integer names, defaults and limits. Aim calibration is
100 times counts per reference pixel at 1080p and 60-degree vertical FOV; it
depends on the current game sensitivity. Aim regions cover the head, neck,
chest, pelvis, body, arms, legs, face, hands and feet. Regions can overlap.

Common diagnostic files include `selftest.log`, `status.log`, `hotkeys.log`,
`wv2_error.log` and `crash.log`. `robrun_runtime.txt` caches discovery hints;
it does not turn an old heap address into a valid address in a new process.

## Interpreting failures

| Observation | What to inspect |
|---|---|
| Self-test fails | Reported configuration, layout, parser or projection assertion |
| Blank menu | WebView2 Runtime, loader DLL and `wv2_error.log` |
| A key does nothing | Input-source entries in `hotkeys.log` and menu/window state |
| No world or implausible roster | Attachment identity, current scene and offset audit |
| Intermittent skeleton | Joint mapping, optional availability and pose age |
| Line of sight stays unknown | Capture freshness, world identity and supported shapes |
| Unexpected exit | `crash.log`; absence of a log is not proof of a clean exit |

The runtime requires a matching game build and driver protocol. Privileged
operations can destabilize Windows, and live-game use may violate game rules.
A successful local write or driver request proves only that operation's
observed result; it does not prove server acceptance or system stability.
Firmware transport is outside this source package and is not a build requirement.
