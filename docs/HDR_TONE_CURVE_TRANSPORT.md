# Native+ tone curve: the extension runs, and above white now reaches the display

## Solved: the carrier was never written (2026-11-03)

The frame was pinned at exactly paper white because of the **carrier**, not the curve. Above-white
left the chain encoded into the *alpha* of the render-size frame (`bb_composite_hdr.frag`), and the
display pass decoded it as `max(alpha - 0.25, 0) * 2`. The game's own composite — the draw the IR
extension actually modifies — never writes that alpha, so the decoded excess was 0 on every frame,
whatever the curve produced. The presented maximum was therefore the brightest value the 8-bit
display image can hold: exactly 1.0, i.e. 202.9121 nits at paper white 203.

The fix is the shape the reference proves (`nativeplus-engine.patch`: its `generated_image` and
`hudless_image` stay RGBA16F, so above white rides in RGB and there is no side channel at all):
the reconstruction's result is written to the output-size RGBA16F image beside the game's 8-bit one,
whole and unclamped (`hdr/hdr_upscale_transfer.comp`), and the display pass takes the larger of the
two (`hdr_output.frag`). The alpha carrier, its hue-preserving scale and its ramp are gone.

Measured on the same harness, `BB_HDR_SCENE_CURVE=1`, DLSS quality, 3840x2160, paper white 203:

| run | frame | panel max | panel avg |
|---|---|---|---|
| before | any | 202.9121 (paper white, pinned) | — |
| after, bright frame | 791.0144 (the configured clamp) | 102.4680 |
| after, dark corridor | 202.9121 | 11.37059 |

The presented maximum now follows the chain's content instead of being pinned at 1.0 — a dark frame
with nothing above white still reads paper white, a bright one reaches the clamp, which is also what
the reference reads (791.01). The run-to-run scene differs, so a like-for-like comparison against
the reference's frame still needs the same spot in the game.

Earlier sections below still hold for the mechanism and the ruled-out list.

## What now works (commit `1e00a7b`)

The extension no longer crashes and it demonstrably rewrites the composite:

- the crash was the push-constant **block type**, not the pass: `EmitContext::DefinePushDataBlock`
  declares thirteen members (the game prefix), while `PushData::ToneCurveIndex` is the struct's
  field order (25). The block now carries `bb_hdr` (128), `bb_hdr2` (144, 160) and `tone_curve`
  (`F32[4]`, 176), and `ToneCurveParam` addresses `PushData::ToneCurveSpvIndex`;
- `extending 5e67d866` → `extended 5e67d866, emitting`;
- `HDR chain draw: ps 5e67d866 -> 0x104ff30000 … float target yes`;
- the curve's parameters reach the shader and change the picture: the scene's average luminance
  moves with `hdr_knee` (2.33 → 4.19 at knee 0.10).

## The wall: the frame's maximum is exactly 1.0

The ReShade panel's max is **exactly paper white in every configuration tried**:

| paper white | knee | diagnostic | panel max |
|---|---|---|---|
| 203 | 0.60 | — | 202.9121 |
| 203 | 0.10 | — | 202.9121 |
| 80 | 0.10 | — | 79.9742 |
| 80 | 0.10 | every above-knee pixel forced to the peak (9.875) | 79.9742 |

The last row is the decisive one: even when the extended shader is told to put every above-knee
pixel at the peak, the presented maximum is still 1.0. So the **presented frame is clamped at 1.0
downstream of the composite** — the extended values are produced but not transported.

That matches what was already established for the old alpha carrier: the post-composite chain's
reads resolve to the guest's 8-bit image (`HDR sample: … read natively (… upscaler-redirected 1)`),
so anything above white is clipped before the display copy. The `RedirectsSampled()` guard is
load-bearing — relaxing it alone presents a 2560x1440 frame inside a 3840x2160 output — so the
light has to be carried *through* the redirect path rather than around it.

Run-to-run caution: the scene's mean varies between launches (pose/time), so **only the panel's
max and the log markers are trustworthy** across runs; mean comparisons between runs are not.

## Next step

Transport, not maths. The composite's target is already a same-size RGBA16F proxy and its draw
reports `float target yes`; what remains is to make the passes after the composite read that proxy
end to end:

1. find which sample resolves the chain's final image to the guest image (the redirect guard in
   `raster_textures.cpp`) and give the chain's own passes a path that keeps the proxy while the
   UI composite keeps the redirected image;
2. re-verify with the panel: the max must leave paper white before any tone tuning is judged;
3. then retire `bb_composite_hdr.frag`, `bb_tonemap_variants.h`, the delta pipeline and `hdr_delta`
   — the IR now does that job.

Known gap to fix before this is shippable: the shader cache holds the extended composite, so
`BB_HDR_SCENE_CURVE=0` cannot turn the feature off until the cache is separated the way the
reference does it (`cache_storage.cpp` keeps extended shaders beside the usual ones).
