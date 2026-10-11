# Windows diagnostic build

The Windows log shows successful mode toggles but no background candidates.
The overlay also switches devices without a corresponding device-creation hook
message. This suggests incomplete interception during the launcher-to-game
handoff, but does not prove it: the original log did not count game draws.

The v1 log establishes that texture setters continue running while draw counters
freeze after initialization. Present reports `Draw-hook=0 DrawIndexed-hook=0`.
Deferred contexts for the game device are captured, ruling out the originally
suspected missing deferred-context creation hook for this run.

The v2 build refreshes context dispatch entries after intercepted texture,
render-target, viewport, and Map calls, and at Present. It saves the current
runtime function whenever an entry changes, so forwarding follows the runtime's
new dispatch selection. Refresh messages are capped at 32 per process.
This targets native Windows runtime dispatch changes; in-game validation is
still required. The correction/classification rules remain unchanged.

Diagnostics include:

V8 installs LoadLibraryA/W and LoadLibraryExA/W import hooks in the Windows
launcher synchronously during initialization. The wrappers patch graphics
imports of the three known game Master modules before returning to their
caller, and propagate loader interception to those modules. Already-loaded
game modules are patched synchronously too. The mod's own loader imports are
never hooked, and resource-only mappings are excluded. Polling remains a
fallback. Wine does not install these loader hooks. Graphics forwarding targets
are saved before the corresponding patched import becomes visible.
The loader_hooks.cpp test loads an inert game-named fixture DLL and checks its
graphics import slots before control returns, including repeated patching.
This does not cover graphics initialization inside a module's DllMain, which
runs inside LoadLibrary before the wrapper regains control.

V7 refreshes native dispatch after IASetVertexBuffers, IASetInputLayout,
IASetPrimitiveTopology, VSSetShader, and GSSetShader. A texture-set refresh is
insufficient if a subsequent glyph vertex-buffer bind replaces the draw entry.
The supplied video shows intact glyphs alternating horizontal positions, and
V6 text-decision logs show consistent NVL classification in intercepted draws.
This supports investigating missed glyph draws rather than further changes to
the inset formula. The regression test simulates a vertex-buffer setter
replacing Draw and verifies restoration of HookDraw with the new runtime entry
saved for forwarding. These additional setter hooks are not installed on Wine.
Real-game confirmation is still needed.

V6 isolates texture bindings and scene classification flags per context on
native Windows. It resets classification at successful FinishCommandList,
rather than letting Present reset an in-progress deferred recording. Wine
continues to use the global state path. Native text decisions are logged for
the first 32 matching font draws. The scene_state.cpp regression test checks
context isolation, preservation across global Present resets, command-list
reset isolation, and Wine fallback. In-game validation remains required.

V4 binds an explicit input layout with the NVL replacement vertex shader on
native Windows, restoring the game's layout after the draw. The log confirms
36-byte font vertices: float3 position at byte 0, float4 color at byte 12,
float2 UV at byte 28. The prior implementation reused a layout created for the
game shader signature. V4 creates the layout using the replacement shader's
bytecode, eliminating that compatibility dependency. This change is native
Windows only; the Proton NVL path and text transforms remain unchanged.
Visual confirmation is still required.

V3 additionally refreshes dispatch immediately before forwarding a corrected
draw, after binding replacement VS/GS state, and after restoring game state.
V2 could call a draw function captured for the pipeline before the replacement
shaders were bound. On native D3D11 that entry can become stale during those
binds, particularly when recording deferred text draws. This is a suspected
cause of the observed text instability; visual confirmation is required.
The added V3 boundaries are enabled only when ntdll does not export
`wine_get_version`. Font shaders, glyph transforms, and Wine/Proton draw
forwarding are unchanged. Proton cannot be executed in this Windows environment.

- A version marker at startup.
- Device ownership and immediate/deferred type for each newly hooked context.
- Actual versus cached textures in slots 0 through 7, viewport, mode, and draw
  type for eight draws per hooked vtable per diagnostic interval.
- Cumulative game draw and texture-set counters, excluding overlay draws.
- Active swapchain/device/immediate-context identity and current hook presence
  every five seconds. Mode changes also dump counters and restart draw samples.

Run a story scene with a stable background, open F12, select 4:3, and leave it
running for at least ten seconds. Close the game before collecting the log.
The log appends to existing sessions; find the diagnostic startup marker.

Interpretation:

- `deferred-create-hook=0` proves that the active device lacks interception of
  deferred-context creation. Existing deferred contexts may already be missed.
- Zero draw counters while the game renders suggest missed contexts or entry
  points; counters alone cannot identify which.
- `actual-textures` differing from `cached-t0` indicates classifier state may be
  stale or mixed between contexts.
- A background in another texture slot, or dimensions different from the
  classifier's 1920-wide and at-least-1400-high rule, explains rejection.
- V1 missing immediate-context hooks at Present indicates the vtable entries
  changed since installation. V2 refreshes those hooks before reporting them.

Build with `./build-diagnostic.ps1` after unpacking the official LLVM-MinGW
20260616 UCRT x86_64 toolchain under `toolchain/`. Outputs are in
`build-windows-diagnostic/`. The DLL statically links its C++ runtime.
