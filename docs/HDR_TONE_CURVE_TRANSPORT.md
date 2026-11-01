# Native+ tone curve: the extension runs; the transport still clamps at 1.0

Status note added after the push-block fix. Earlier sections below still hold
for the mechanism and the ruled-out list.

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
