# Background camera motion in 4:3 mode

The 16:9 recordings show zoom and pan that are absent with the current 4:3
correction. The two supplied screenshot pairs show the same problem in the
clothing store and bedroom. This is explained by `BackgroundVS` in `mod.cpp`:
it replaces the game's background vertex shader with a static four-corner quad
and fixed `0..1` texture coordinates. That reveals the original 4:3 image, but
throws away the engine's camera transform.

An **opt-in experiment** now keeps the game's vertex shader and modifies its
output with a geometry shader inside the corrected 4:3 viewport. A 16:9 crop
can be expressed either as a vertically cropped UV range (about 75% of the
horizontal range), or as a full-texture quad extending beyond the original
viewport (about 4:3 taller in clip space). The experiment detects these two
patterns per triangle and respectively expands the vertical UV range or
reduces the vertical clip-space extent. Zoom and pan already present in the
original vertices are retained. Unrecognised geometry passes through without
an additional transform.

For a CG whose 16:9 crop is already close to the top or bottom of the source
texture, vertical expansion may otherwise sample outside the image and smear
an edge row. The motion shader shifts such an expanded crop back inside the
source bounds while preserving its size; camera movement continues until the
edge of the 4:3 artwork is reached.

This is **not yet validated across the game**. An inventory of the May and
Maybe `bg.mpk` archives found 692 images at 1920x1440, six at 1920x1441,
and two at 1920x1439. The motion path covers all three near-4:3 sizes, plus
the observed 1920x2560 scrolling CG whose 16:9 UV window moves vertically
through the tall image. The archives also contain other tall sizes (up to
1920x5400), including animation strips; their frames cannot safely be
treated as one continuous 4:3 image without scene-specific evidence. Those
sizes, draws with an existing game geometry shader, and unrecognised geometry
retain the established fixed-quad path. Leave the option disabled if it
causes missing backgrounds or a wrong crop.

To test, set these values in `MyMerryMayFix.ini` and restart the game:

```ini
[Graphics]
AspectMode=1
ExperimentalBackgroundMotion=1

[Debug]
Logging=1
BackgroundMotionDiagnostics=1
```

Compare the two supplied scenes in 16:9 and 4:3 while their camera animation
is running. The diagnostic logs the original background quad and vertex-shader
constant buffers roughly every 120 ms, up to 180 samples per capture. Opening
the F12 overlay resets that limit; close it immediately before the scene to
capture the relevant camera movement. It logs in both aspect modes, so one Off
capture and one 4:3 capture are useful. Save
the log after each run before relaunching, and include a screenshot or short
video marking where zoom/pan diverges. `Background geometry VB0 vertex0..3`
shows the raw streamed vertices; `VS-CB0/1` helps identify camera transforms
performed in the game's vertex shader. If `snapshot=0`, that buffer was not
available to the current CPU-side diagnostic and needs a different capture
method. Disable `BackgroundMotionDiagnostics` and `Logging` after testing.
