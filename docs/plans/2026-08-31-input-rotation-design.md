# Input rotation

Rotate a source 90/180/270° clockwise before display, per input, from the
CLI or interactively.

## Motivation

Comparing two files only works when both show the scene the same way up. A
master and a re-encode that disagree on orientation cannot be put side by
side at all — one pane is sideways and every pixel comparison is
meaningless.

hdrplay has never handled rotation in any form. `pl_map_avframe_ex`
(`renderer.c:636`) leaves `pl_frame.rotation` at zero and nothing else in
`src/` mentions it.

## Non-goal: automatic rotation from the display matrix

libavutil exposes container rotation as `AV_FRAME_DATA_DISPLAYMATRIX`
side data, and QuickTime, ffplay and mpv all honour it. hdrplay could too,
in about five lines at `source_open`.

It is deliberately out of scope. The file that motivated this work carries
no rotation metadata — the orientation is wrong in the pixels, with nothing
in the container to say so. Automatic handling would not have helped, and
adding a second, invisible source of rotation makes the manual flag harder
to reason about: you would have to know whether a given `--rotate 90`
composes with a tag or replaces it.

If a tagged file does turn up, the plumbing this design lands makes
autorotate a small follow-up: read the matrix in `source_open`, seed
`Renderer.rotation[i]` from it, and decide then whether `--rotate`
overrides or composes.

## CLI

```
--rotate [N:]DEG    rotate an input DEG degrees clockwise before display.
                    DEG is 0, 90, 180 or 270. Bare `--rotate 90` applies to
                    every input; `--rotate 1:90` applies to the second file
                    only. Inputs are numbered from 0.
                    Repeatable: `--rotate 0:90 --rotate 1:270`.
```

Inputs are numbered from 0, following ffmpeg's stream specifiers
(`-map 0:v`) and matching `-d INDEX`, which is already documented as
0-based. The `N:` prefix also extends without a new flag name if a third
input is ever allowed, which `--rotate-a` / `--rotate-b` would not.

This does *not* agree with the `1`/`2` solo keys, which are 1-based:
`--rotate 1:90` rotates the file that `2` solos. The keys are a fixed cost
— they predate this work and renumbering them would break muscle memory
and every existing invocation — so the choice was which side of the fence
`--rotate` sits on. It sits with the rest of the CLI, where `-d` has
established 0-based indexing and where ffmpeg sets the expectation for
anyone reaching for a per-input flag. The mismatch is documented in the
usage text and the README rather than papered over.

Rejected: ffmpeg-style sticky per-input options, where `--rotate` binds to
the next positional argument. It is the most expressive form, but every
other hdrplay flag is order-independent today and making one flag
order-sensitive is a trap.

Bad degrees, an index outside 1–2, or a malformed operand print to stderr
and exit 2, matching how `--sdr-gamut-map` rejects an unknown mode
(`main.c:404`).

### Interactive

**T** rotates the focused source 90° CW per press, wrapping at 360.
`renderer_focus_source()` (`renderer.c:610`) already defines focus, so with
two files un-soloed T hits pane A; press `2` to solo the second file, then
T. Logs `rotate src1 -> 90` the way the other mode switches log.

T is free. Taken: Q F H S P O L R M I A X Z 0 1 2, space, arrows, comma,
period, plus/minus.

## State

`int rotation[2]` on `Renderer`, in degrees clockwise, normalized to
{0, 90, 180, 270} at parse time.

It lives on `Renderer` rather than `Source` because it is view state, not
decode state: T mutates it per frame, and every other thing T-like already
lives there (`mode`, `solo`, `swapped`, `zoom`, `pan_x/y`), initialized in
the same block of `main.c` assignments. `Source` stays a pure decode-side
struct.

## Touch points

Three, and the third is the one that is easy to miss.

### 1. Render

`renderer.c`, immediately after `pl_map_avframe_ex` succeeds:

```c
r->slot[i].image.rotation = pl_rotation_normalize(r->rotation[i] / 90);
```

libplacebo samples rotated for us. The header pins the direction:
"if this is `PL_ROTATION_90`, then the image will be rotated to the right
by 90° when mapping to `crop`" (`renderer.h:600`) — right meaning
clockwise, which is the sense `--rotate` documents.

`image.crop` stays in unrotated source space, because libplacebo rotates
*after* cropping, so `layout_image_crop_ref` needs no change at all.

Set at map time rather than in the per-pass loop. Intermediates
(`render_to_intermediate`, used by SDR mode and by source B in a DIAG
wipe) render straight from `r->slot[si].image` before the pass loop runs,
so a pass-local assignment would rotate the direct path and leave those two
upright.

### 2. Layout dimensions

A new pure helper in `layout.c`, public so `test_layout.c` can reach it:

```c
void layout_rotated_dims(int rot, int w, int h, int *ow, int *oh);
```

90/270 swap width and height; 0/180 pass through.

Two callers:

- `renderer.c`, filling `LayoutInput.src_w` / `src_h` from
  `shown->width/height`. Layout uses these for the zoom reference and for
  `layout_reference_source`, so they must describe the frame as displayed.
  Feeding rotated dims in means the reference is picked by displayed area
  and zoom stays consistent across a rotated/unrotated pair, so both panes
  keep covering the same region of the scene.
- `main.c`, where the portrait→top/bottom split default currently tests
  `sources[0].dec.height > sources[0].dec.width`. That reads the
  orientation the encoder stored, not the one the user will see. A
  landscape-stored file rotated to portrait should flip the default split
  to top/bottom, and today it would not.

### 3. Probe

`renderer.c` maps a window fraction back to a source pixel so the probe can
report nits under the cursor. The inverse rotation has to be applied to
`(fx, fy)` before the crop lerp, or the readout samples the wrong pixel —
silently, with a plausible-looking number.

The inverse map itself lives in `layout.c` as

```c
void layout_unrotate_norm(int rot, double *x, double *y);
```

rather than as a static in `renderer.c`. It is the same geometry as the
dimension swap, and `renderer.c` cannot be reached from the tests — which
matters more here than anywhere else, since a transposed axis produces a
wrong number rather than a visible defect.

## Testing

Three cases in `test_layout.c`.

`layout_rotated_dims`: the swap at each of 0/90/180/270, plus 450 and -90
for the normalization (the T key increments without wrapping until read
back).

`layout_unrotate_norm`: round-tripped against a forward map written out
separately in the test, so the assertions do not just restate the
implementation. Plus two explicit direction checks — at 90° the frame's
top-left must appear in the window's top-*right* — because a
counter-clockwise implementation passes the round trip and fails reality.

`layout_plan` invariance: a 1080x1920 source at 90° must produce a
byte-identical plan to a native 1920x1080 source. That is the whole claim
the dimension swap is making.

The existing layout assertions stay green untouched. Rotation never reaches
`layout_plan`; it only changes which dimensions are handed to it. That is
precisely the regression guarantee the header comment in `layout.h` exists
to protect, so leaving those tests unmodified is the point rather than an
oversight.

## Not verified automatically

The on-screen direction. `layout_unrotate_norm`'s tests pin the convention
the probe assumes, and the libplacebo header pins the convention
`image.rotation` implements, but nothing in CI confirms the two agree —
that needs a GPU and eyes. To check by hand:

```bash
ffmpeg -y -f lavfi -i "color=c=red:s=320x180:d=3" \
       -f lavfi -i "color=c=lime:s=320x180:d=3" \
       -f lavfi -i "color=c=blue:s=320x180:d=3" \
       -f lavfi -i "color=c=white:s=320x180:d=3" \
       -filter_complex "[0][1]hstack[t];[2][3]hstack[b];[t][b]vstack,format=yuv420p" \
       -c:v libx264 -t 3 /tmp/rot_test.mp4

hdrplay --loop /tmp/rot_test.mp4               # red lime / blue white
hdrplay --loop --rotate 90 /tmp/rot_test.mp4   # blue red / white lime
```
