# ADV wrapping investigation

The original game archives remain unchanged. Build v9 includes an optional,
native Windows experiment controlled by `MyMerryMayFix.ini`.

## Finding

Both supplied Master DLLs contain matching engine routines that compare an
accumulated line width plus the next item's advance against a caller-supplied
width, then enter break-selection logic. This is a strong candidate for the
engine wrapping function, not yet a live-verified hook target.

| Module | Candidate RVA | Layout-record array RVA | Width field |
| --- | --- | --- | --- |
| MyMerryMay_WIN_Master.dll | 0xCFC70 | 0x12E8F40 | unsigned 16-bit at record +0x14 |
| MyMerryMaybe_WIN_Master.dll | 0xCFE80 | 0x12E9F80 | unsigned 16-bit at record +0x14 |

Record stride is 0x30 bytes in the observed callers. Both array addresses,
function RVAs, and argument patterns were independently located in the DLLs.
Addresses are relative to each loaded module, not fixed process addresses.
Version fingerprints and all six observed callers per game are recorded in
`diagnostics/adv-wrap-evidence.json`. Complete routine disassembly is in
`diagnostics/may-wrap-function.txt` and `diagnostics/maybe-wrap-function.txt`.

For May, the routine stores its first integer argument (ECX) in the local width
limit at stack +0x70. At RVA 0xD02B5 it adds local item advance (+0x58) to
accumulated width (+0x30); at 0xD02C1 it compares that result with the limit.
The overflow path examines per-item flags, including adjacent items, rather
than performing an unconditional fixed-character-count break. The paired
preprocessing routine at 0xCF770 computes these flags from message items and
character tables. Preserving these routines is preferable to implementing a
second wrapping algorithm in the D3D hooks.

Callers load the width from record +0x14. Some callers also pass a signed value
from record +0x04 as the second argument. Its meaning is not confirmed, so it
must be preserved. Several callers use record index 6; others use a variable
index. It is not safe to globally replace the width of record 6 or patch every
call: dialogue, names, NVL, and system UI may share this machinery.

## Data format

Both games' `mes00.mpk` archives use MPK headers and 256-byte directory entries.
The inspected message members begin with `MES`, version 1, and a message count
and text-section offset. For example, May's `ma_begin.msb` contains 324 entries
with text starting at 0xA30, and Maybe's `mb_a01.msb` contains 164 entries with
text starting at 0x530. These offsets are consistent with a 16-byte header and
8-byte ID/offset entries. Script execution is stored separately in `.scx`
members of `script.mpk`. No game archive has been repacked or modified.

Existing MAGES script tools provide useful background:
https://github.com/CommitteeOfZero/sc3tools
This game is not listed among that tool's supported games, so this investigation
does not assume its text-control decoder is compatible with these messages.

## Proposed implementation

1. Instrument the six wrap callers to log caller RVA, width, record index where
   available, second/third arguments, and resulting line count. Capture the
   same long ADV message, NVL, speaker name, and a system screen in each game.
2. Identify the ADV body-text caller/layout record using those captures. Work
   before glyph generation, not after glyphs have already been positioned.
3. In 4:3 mode only, pass a smaller width to that caller while leaving glyph
   advances, font size, vertical spacing, and the engine's break-selection
   logic untouched. Compute the width from the corrected text area's bounds
   and actual starting X; blindly multiplying by 0.75 may mishandle margins.
4. Verify that the extra lines fit the ADV panel. Confirm whether the engine
   provides automatic continuation/page overflow and whether explicit script
   newlines consume additional rows. Do not silently place extra lines under
   AUTO/SKIP or outside the panel.
5. Validate punctuation, ruby, typewriter reveal, voice timing, skip/auto,
   backlog, save/load, mode toggles, and both games. Keep NVL/name/system callers
   untouched. Initially gate the experimental hook to native Windows so the
   validated Proton path remains unchanged.

The experimental hook checks the routine's 21-byte prologue and all six direct
call targets, and skips unsupported modules. It redirects those calls through
an executable relay, preserving the engine routine and its unwind metadata.
Runtime caller/mode identification and vertical-overflow behavior still need
testing in the game; this is not a verified ADV-only fix.

## Testing build v9

In the INI beside `dinput8.dll`, use:

```ini
[ExperimentalWrapping]
Enabled=1
Width=1320
Caller=0
```

Restart after each INI change. Enable 4:3 in the overlay. Width is a maximum
in engine layout units, not screen pixels; 1320 is a starting test value.
The engine retains its glyph advances and break-selection logic. An existing
width smaller than this maximum is left unchanged. `Width=0` installs hooks
and logs calls without changing any widths. `Caller=0` tests all six callers;
`Caller=1` through `6` changes only that caller. Other callers are still logged.
Invalid widths become observation-only; invalid caller values select no caller.

Logs report game, caller number/RVA, original/adjusted width, the two preserved
arguments, and aspect mode (first 32 calls per caller per launch). Use separate
launches for the long ADV line, NVL, speaker names, menus, and backlog. Match
the caller to each scene, then restrict the override to the ADV caller. Check
whether extra lines fit the panel, including AUTO/SKIP and manual line breaks.
Neither the second/third argument meanings nor all caller roles are confirmed.

`Enabled=0` (default) disables the entire experiment. Linux/Proton always skips
it, regardless of INI settings. Aspect changes affect subsequent layout calls;
advance the dialogue after changing aspect mode to obtain a newly wrapped line.

## Build v10: restrict wrapping to ADV

The live May log identifies two layouts on the same caller (1):
ADV `(width=1420, arg2=285, arg3=0)` and NVL `(1620,180,0)`.
Caller 4 uses width 1500 with zero origin during the tested backlog path.
Therefore selecting caller 1 alone cannot isolate ADV. Build v10 applies
Width only to caller 1 with the observed ADV argument tuple. All other calls
retain their original widths, even when Caller=0. Unknown layouts fail closed.
The paired Maybe hook uses the same conservative check; a different Maybe
ADV tuple will remain unchanged until its log identifies it.

The root INI is enabled for testing with Width=1320, Caller=0. Enabled=0 and
Width=0 still disable changes. Caller=1 has the same effect as Caller=0;
Caller=2..6 changes no text because those paths are excluded by the ADV gate.
The prior build v9 all-caller instructions above describe the investigation,
not the behavior of build v10. Font rendering and the Proton path are unchanged.

### NVL left edge investigation

The supplied first NVL screenshot starts at about logical X=180 (screen
X=192 at a 2048-wide client area). The existing native NVL correction maps
logical X by `0.85*x + 128`, which would put X=180 near 281, inside the
4:3 area's left edge of 240. The screenshot is therefore consistent with
this passage missing the usual NVL correction, rather than width capping
moving its start position. Width capping only changes the first argument
of the wrapping routine, never the origin argument.

May's renderer selects the NVL correction only after a 512x923 panel texture
marks the current scene as NVL. A marker-free passage, or a font draw before
that marker, can consequently use the ordinary text path. This remains a
hypothesis until the passage's draw state is captured. Build v10 samples each
NVL/message/system marker combination separately and logs vertex geometry
for font draws with no NVL marker; launch-time ADV draws no longer exhaust
all samples before the unusual NVL scene. No position correction was changed
based on an unverified diagnosis. Extreme-width vertical overflow remains
outside this change, as requested.

## Build v11: panel-free NVL positioning

The focused v10 capture selects the unchanged NVL layout at 17:52:18.129:
caller 1, width 1620 -> 1620, origin 180. The overlay's draw trace at
17:52:29.297 onward shows background, 4096x416 UI, 4096x460 navigation, and
4096x7546 glyphs, with no 512x923 panel in the captured sequence. The first
glyph vertex has X=-0.8197917, Y=0.8074074. X is outside the left pillar
boundary (-0.75). The existing NVL GS would map it to approximately -0.71349,
inside the content area. ADV's GS leaves upper-screen text unchanged.

Build v11 additionally selects May's existing native NVL geometry shader
when the engine's active body layout is the observed NVL tuple
(width=1620, origin=180, third argument=0). The body-layout observer updates
only on caller 1; backlog callers leave that state intact, and an ADV or
unknown body layout clears it. Existing system-UI and message-UI guards still
apply. This uses the same correction as panel-marked NVL, not a new font or
spacing transform. Maybe's existing shader path is unchanged.

The native layout observer installs independently of ExperimentalWrapping
Enabled. Enabled=0 disables the ADV width override but retains this NVL fix;
Width=1320 still affects only the observed ADV tuple. Wine/Proton installs
neither the observer nor these native hooks. Unsupported code signatures skip
the observer. Diagnostics now include engineNVL and give its states separate
sample budgets. Tests cover NVL/ADV transitions, backlog preserving mode,
width-disabled observation, Wine bypass, argument forwarding, and real PE
hook signatures. The corrected in-game scene still requires visual testing.

## Build v12: apply the same NVL correction during reveal

The v11 log shows engineNVL=1 at 17:57:06.832, but reveal context
0000012050EFD880 has message=0 and GS=null. Completed-text context
00000120693F7980 subsequently has message=1 and the NVL GS bound.
The v11 selection still required messageUiSeen in addition to the engine cue,
so it missed reveal draws without UI in their deferred recording.

Build v12 lets the native engine NVL cue select the existing correction
without a message-panel draw. The panel-based selection remains unchanged
for Proton. Outer draw guards (aspect, background, system UI, font texture,
vertex count and topology) still apply. A regression check covers two contexts
with differing panel state choosing the same native NVL correction, and Wine
ignoring the native cue. ADV width settings and backlog wrapping are unchanged.

## Build v13: share NVL detection with Proton

The body-layout observation and May NVL selection now run on both Windows
and Wine/Proton. This applies the panel-free/reveal fix to the shared game
layout while keeping each platform's existing correction shader: native
Windows uses NativeNvlGS; Proton uses its established NvlFontVS. Native
loader interception, draw-dispatch refresh and context routing remain
Windows-only. The ADV width override also remains Windows-only.

Enabled=0 still disables only the ADV width experiment. The observer validates
routine/caller signatures on both platforms. Automated tests exercise the
Wine-selected observation/selection branches, unchanged width arguments,
and both supplied module signatures. Actual Proton gameplay could not be
run in this Windows environment and needs a visual regression check.
