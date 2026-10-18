# My Merry May With be Fix

[日本語版 README](README.ja.md)

Vibe coded proxy fix for the PC port of **My Merry May With Be** adding 4:3 original aspect ratio and basic QOL features the engine misses. 
All CGs and BGs are heavily cropped in this remaster version of the game to fit into 16:9 aspect ratio, but original files are still in the game assets.
Comes with a configurable .ini and a in game overlay to change settings in real time.

Features:
- Overlay: Press F12 to open.
- Display Mode: Change between 4:3 and 16:9.
- Textbox settings: Set opacity and colors.
- Sprites over bars: Keeps full width tachie to avoid losing detail in 4:3 display mode.
- Voice text speed: Disable the default text speed synced to voice lines.
- Voice continuation: Keep voice playback to next line until the next voiced line.
- High-resolution rendering: Renders the text and interface at main display resolutions instead of 1080p.
- Mouse cursor restoration: Fixes weird mouse interaction of the game when focusing the game or pressing a keyboard input.
- Optional XInput controller bridge: Maps basic controller buttons to the game's DirectInput keyboard controls.
- ADV wrapping: Allows to change the length of ADV textbox lines. Needed for 4:3 where some long sentences go above the black pillarbox.

Most story backgrounds and CGs are already stored as 4:3 assets, but the PC port crops them for a 16:9 presentation.
The fix watches D3D11 texture bindings and identifies those original 4:3 images by their dimensions and render sequence.
Matching story draws receive full-image texture coordinates and a centered 4:3 viewport with black pillarboxes.
ADV and NVL text/UI are recognized from their texture atlases and geometry, then repositioned to fit the restored image.
The active story layout is retained across the port's black/backgroundless frames; its fullscreen white story frames are also confined to the 4:3 image area.
The game selector and known widescreen system screens and menus are excluded so their native layouts remain unchanged.

This experimental 64-bit `dinput8.dll` proxy covers both **My Merry May** and
**My Merry Maybe**. It forwards DirectInput to the real Windows/Wine DLL, hooks
the game's D3D11 swap chain, and displays a small diagnostic overlay.

## Install

Place these two files in the root of the game's folder:

- `dinput8.dll`
- `MyMerryMayFix.ini`

Keep them beside `MyMerryMayWithbe.exe`.

### Proton
In Steam, open the game's Properties and set this Launch Option:

```text
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

If `WINEDLLOVERRIDES` is already used for another mod, merge `dinput8=n,b` into
that existing value instead of adding a second assignment.

Start the game and press **F12**. Use **Up/Down** to select an option and
**Left/Right** to change it. Use **Escape** or **F12** to close the overlay.
All changes are saved to `MyMerryMayFix.ini`.

While the overlay is open, keyboard, mouse, and DirectInput device state are
suppressed before they reach the game. This prevents overlay navigation keys
from advancing the story during testing. Press **F12** again to close it.

To uninstall, remove the override and move or delete `dinput8.dll` and
`MyMerryMayFix.ini` from the game directory.

## Display modes

- **Off**: Remaster's default 16:9 mode.
- **4:3 pillarbox**: detects the game's 1920-pixel-wide,
  4:3 background textures, replaces their cropped quad with a full-UV quad,
  and renders them into a centered 4:3 viewport. Known message-window UI sheets
  are also confined to that viewport. Both games' ADV panel variants
  (`4096x460` and `4096x464`) are recognized. NVL text is proportionally inset by a
  dedicated vertex shader, while an ADV geometry shader moves complete
  speaker-name triangles onto the corrected nameplate without shearing glyphs.
  My Merry Maybe's marker-less NVL layout is identified from its upper-screen
  font geometry and receives the same proportional inset independently of ADV.
  Its duplicate background draws are corrected together while later effect
  passes that retain the same texture binding are left on their original shader.
  Save/load, configuration, gallery, backlog, title/map, and confirmation-dialog
  texture sheets from both games are recognized as system UI and bypass the
  story-layout corrections.
  Transitions, animated backgrounds, and the gallery need real-game testing
  before this mode can be considered safe.

Pillarboxes are always black in this build.

## ADV textbox style

The overlay provides live opacity and RGB target-colour sliders for the ADV textbox.
Opacity ranges from 0 through 200 percent, with 100 percent preserving the
original texture alpha. A non-white RGB value recolours the panel while
retaining its original light/dark gradient; `255,255,255` preserves the exact
original teal rendering. Hold Left/Right to adjust any slider continuously.
Changes are saved immediately:

```ini
[Graphics]
TextboxOpacity=100
TextboxColorR=255
TextboxColorG=255
TextboxColorB=255
HideAutoSkipButtons=0
UniformTextboxOpacity=0
```

`HideAutoSkipButtons=1` hides the ADV AUTO/SKIP graphics in both games, including
their highlighted states. The switch is available live in the overlay and is saved
to the INI. The replacement shader is disabled at the defaults above. Opacity
and colour affect only the ADV textbox region, including its message-transition
copies; the hide switch affects only the button images. Speaker chrome, ADV
glyphs, NVL, backlog, and system menus retain their original style.

The bottom gradient shown in some narration scenes is also part of the ADV
panel texture. `UniformTextboxOpacity=1` (also in the overlay) replaces that
texture's vertical alpha gradient with a uniform alpha. In this mode the
opacity slider maps 0–200 to 0–100% actual opacity; 100 gives 50%. Transparent
panel edges and the game's transition fades are retained. This targets the
ADV panel in both games, not NVL or other UI artwork.

## Sprites over bars
`SpritesOverPillarbox=1` (the default) preserves the PC port's extra widescreen
character art by allowing sprites to extend over the black side regions.
Disabling it will hide part of the characters so I recommend to leave it on.

## Voiced text speed

Voiced dialogue normally receives a voice-derived reveal duration, making it
slower than unvoiced dialogue even at the same in-game text-speed setting. The
overlay option **Voiced text uses normal speed** disables only that duration
override, so voiced and unvoiced lines use the same configured reveal speed.
Voice playback and auto-mode timing are not changed. The setting is live and
is saved as:

```ini
[Text]
MatchVoicedTextSpeed=0
```

Set it to `1` to enable matching or `0` to retain the original voice-synced
text reveal.

## Voice continuation

Normally, advancing a line immediately stops its voice playback. The overlay
option **Keep voice until next voiced line** suppresses only the verified
message-advance stop on the game's voice channel. The current voice can then
finish while unvoiced lines are displayed; when another voiced line begins,
the engine replaces the previous voice through its normal playback path.
BGM and sound effects are not affected. The setting is live and is saved as:

```ini
[Audio]
ContinueVoiceUntilNextVoice=0
```

Set it to `1` to enable continuation. Explicit stops outside the message
advance routine retain their original behavior.

## High-resolution rendering

The game normally renders the complete scene and its text into a fixed
1920x1080 intermediate, then scales that texture to the fullscreen back buffer.
The experimental rendering option enlarges matching render/depth intermediates
and their viewports while leaving normalized scene and UI geometry unchanged:

```ini
[Rendering]
HighResolution=1
Width=0
Height=0
```

`Width=0` and `Height=0` use the active monitor resolution. Set both to an
explicit size such as `3840` and `2160` to render at that resolution instead.
The option has no effect when the selected target is not larger than 1080p.
Restart the game after changing it. This is experimental because transitions
or effects may use fixed-size resources that have not yet been observed.

Improves:
- ADV, NVL, speaker-name and menu text, because the large font atlases are rasterized directly at the higher resolution.
- Character-sprite edges when the source artwork has more detail than 1080p could display.
- UI elements, textbox edges, gradients and other high-resolution interface textures.
- Transitions and shader effects rendered into the enlarged intermediate target.
- Pillarbox edges and positioning precision.

## Overlay status

The panel shows whether experimental high-resolution rendering is enabled and,
when active, its effective internal resolution. Detailed D3D11 diagnostics are
kept in the log instead of occupying space in the overlay.

If Wine reports an unexpected monitor size, enable `Logging=1` and
`DisplayDiagnostics=1` in `[Debug]`, then restart. The log compares Win32
monitor modes, active display configuration, DPI, window size, and DXGI output
at startup and swapchain creation. These checks are read-only.

For misaligned system confirmation text, `SystemDialogDiagnostics=1` in
`[Debug]` logs up to 256 draw calls after F12 opens. Capture the same box once
in 16:9 and once in 4:3; the trace records texture, viewport, and text-quad
positions without changing the image.

`MyMerryMayFix.log` is created beside the DLL. When reporting a problem, include
that log and screenshots with the overlay open in both story mode and gallery
mode. For background classification, a screenshot taken while the relevant CG
is stable is more useful than one taken mid-transition.

The overlay uses keyboard arrow keys controls.

### Change the hotkey

Edit `MyMerryMayFix.ini` while the game is closed:

```ini
[Overlay]
Hotkey=F12
```

Supported values are `F1` through `F12`, `INSERT`, `HOME`, `END`, and `DELETE`.

## Mouse cursor restoration

How the game works: 
When you focus the game after alt-tabbing or similar, or press any keyboard input the mouse 
disappears requiring you to do a left click to show the mouse again. This option changes this 
behaviour so when you move the mouse it auto restores without the need to do a left click.

When the DirectInput mouse reports `DIERR_INPUTLOST` or `DIERR_NOTACQUIRED`, the
fix defers reacquisition while the game is inactive. It retries from the game
window thread as soon as focus or mouse movement returns, then retries the
state read. The DLL also remembers the game's custom cursor before keyboard or
focus changes and restores that exact cursor on the next mouse movement. Until
the next real click, it prevents the engine from immediately hiding or clearing
that cursor again. The first movement also supplies an unused middle-button
DirectInput pulse so the engine can leave keyboard mode without consuming the
player's first left click. It never substitutes the generic system arrow.
Disable this behavior and restore the original handling with:

```ini
[Input]
RestoreCursor=0
```

## XInput controller bridge

Without Steam Input, the game does not natively use the available XInput
controller. To enable the optional bridge, restart after setting:

```ini
[Input]
XInputEnabled=1
XInputNativeActions=1
```

With `XInputNativeActions=1`, the first connected pad feeds the bundled Steam
Input actions to the game. A/B/X/Y, Start, bumpers, and D-pad use the VDF's
confirm/back/auto/backlog/menu/skip/navigation actions. The left stick provides
arrow navigation. Set `XInputNativeActions=0` to use the configurable keyboard
and mouse bindings below. The game’s keyboard guide lists Space=advance,
X=cancel/textbox, G=auto, 2=backlog, 1=system menu, Q=force skip, E=skip mode,
R=quick save, and F=quick load. The default LT/RT bindings provide quick
save/load even while native actions are enabled; triggers activate past the
XInput threshold. Set any binding to `NONE` to disable it, or use `RIGHTMOUSE`
or `LEFTMOUSE` to send a mouse button. For example, change `B=RIGHTMOUSE` to
`B=X` to use the X keyboard shortcut instead.

```ini
[Input]
XInputEnabled=1
XInputNativeActions=0

[Controller]
A=SPACE
B=RIGHTMOUSE
X=G
Y=2
Start=1
Back=X
LB=Q
RB=E
LT=R
RT=F
L3=NONE
R3=NONE
Up=UP
Down=DOWN
Right=RIGHT
Left=LEFT
```

Bindings are read at startup. Physical keyboard input is preserved;
controller input is ignored while the overlay is open or the game is
unfocused. This works on Windows and Proton if XInput exposes the pad.
`[Debug] ControllerDiagnostics=1` logs controller visibility and button activity
when `Logging=1`.

## Experimental ADV wrapping

The overlay can turn ADV wrapping on/off and adjust its line width while the
game runs. Changes are saved to `[ExperimentalWrapping]` and apply to the next
dialogue line; already displayed text is not reflowed. Hold Left/Right to
adjust by 5 units, or Shift+Left/Right for single-unit steps. The overlay
allows widths from 800 to 1420. NVL, backlog, speaker names, menus, and other
layouts retain their original widths. Direct INI edits still require a restart.

```ini
[ExperimentalWrapping]
Enabled=1
Width=1320
```

For a crash trace, set `[Debug] Logging=1` and `WrappingDiagnostics=1`, then
restart and replay the line. The log records each ADV wrapping call and, on
the known May crash path, the engine's glyph index and line-width state.

## Build on Linux

Requirements: CMake, Ninja, and the 64-bit MinGW-w64 C++ compiler.

```sh
cmake -S . -B build-mingw -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw64.cmake
cmake --build build-mingw
```

The result is `build-mingw/dinput8.dll`. No third-party source dependency is
downloaded; the overlay uses D3D11 and a small GDI-backed text texture.
