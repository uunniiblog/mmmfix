# My Merry May With be Fix

[日本語版 README](README.ja.md)

Vibe coded proxy fix for the PC port of My Merry May With Be adding 4:3 original aspect ratio and basic QOL features the engine misses. 
All CGs and BGs are heavily cropped in this remaster version of the game to fit into 16:9 aspect ratio, but original files are still in the game assets.
Comes with a configurable .ini and a in game overlay to change settings in real time.

Features:
- Overlay: Press F12 to open.
- Display Mode: Change between 4:3 and 16:9.
- Textbox settings: Set opacity and colors.
- Sprites over bars: Keeps full width tachie to avoid losing detail in 4:3 display mode.
- Voice text speed: Disable the default text speed synced to voice lines.
- Voice continuation: Keep voice playing to next line until next voiced line.
- High-resolution rendering: Renders the text and interface at main display resolutions instead of 1080p.
- Mouse cursor restoration: Fixes weird mouse interaction of the game when focusing the game or pressing a keyboard input.
- ADV wrapping: Allows to change the length of ADV textbox lines. Needed for 4:3 where some long sentences go above the black pillarbox.

Most story backgrounds and CGs are already stored as 4:3 assets, but the PC port crops them for a 16:9 presentation.
The fix watches D3D11 texture bindings and identifies those original 4:3 images by their dimensions and render sequence.
Matching story draws receive full-image texture coordinates and a centered 4:3 viewport with black pillarboxes.
ADV and NVL text/UI are recognized from their texture atlases and geometry, then repositioned to fit the restored image.
The active story layout is retained across the port's 16:9 black/backgroundless frames, keeping their UI in 4:3.
Known widescreen system screens and menus are excluded so their native layouts remain unchanged.

This experimental 64-bit `dinput8.dll` proxy covers both **My Merry May** and
**My Merry Maybe**. It forwards DirectInput to the real Windows/Wine DLL, hooks
the game's D3D11 swap chain, and displays a small diagnostic overlay.

## Install

The repository root already contains the two files needed by the game:

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
Display, sprite, opacity, and tint changes are saved to `MyMerryMayFix.ini`.

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
```

The replacement shader is disabled at those defaults. At other values it is
restricted by source texture coordinates to the full-width ADV textbox region,
including its message-transition copies; AUTO/SKIP, speaker chrome, ADV glyphs,
NVL, backlog, and system menus retain their original style.

## Sprites over bars
Choose if you want the character tachie to go above the black pillarboxes in 4:3 mode.
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


`SpritesOverPillarbox=1` (the default) preserves the PC port's extra widescreen
character art by allowing sprites to extend over the black side regions. Set it
to `0`, or toggle **Sprites over bars** in the overlay, to composite the black
bars over character sprites like the original 4:3 release. This does not crop
or modify the underlying sprite assets.

## Overlay status

The panel shows whether experimental high-resolution rendering is enabled and,
when active, its effective internal resolution. Detailed D3D11 diagnostics are
kept in the log instead of occupying space in the overlay.

`MyMerryMayFix.log` is created beside the DLL. When reporting a problem, include
that log and screenshots with the overlay open in both story mode and gallery
mode. For background classification, a screenshot taken while the relevant CG
is stable is more useful than one taken mid-transition.

The overlay deliberately uses keyboard controls because the game's mouse input
is one of the later issues this project is intended to fix.

## Change the hotkey

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

## Experimental ADV wrapping

The optional `[ExperimentalWrapping]` settings work on both native Windows and
Proton. With `Enabled=1`, the fix reduces only the verified ADV dialogue-body
layout to `Width=1320` engine units so long lines wrap before the right
pillarbox. NVL, backlog, speaker names, menus, and other layouts retain their
original widths. Restart the game after changing these values.

```ini
[ExperimentalWrapping]
Enabled=1
Width=1320
Caller=0
```

Modifying the width can break the textbox or make text disappear so while you can play
with the value, I recommend leaving it around 1320.

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
