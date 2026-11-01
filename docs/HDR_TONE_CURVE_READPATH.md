# Native+ tone curve: the read path cannot carry it (three attempts)

Appended after three attempts to make the chain's float proxy reach the display, all of which
broke the composition in the same way. Read with `HDR_TONE_CURVE_TRANSPORT.md`.

## The three attempts

The composite's extended output is provably in its float proxy (`float target yes`, and the scene
responds to `hdr_knee`), yet the presented maximum is exactly 1.0 in every configuration. The
passes after the composite sample the guest's 8-bit image, so the light dies there. Three ways of
routing the proxy instead:

| attempt | change | result |
|---|---|---|
| 1 | the redirect guard relaxed for every pass | presented 2560x1440 inside a 3840x2160 frame |
| 2 | the same, but only outside the upscaler's UI phase (`InUiPhase()`) | same regression |
| 3 | the same, but only for the display copy (`stage.pgm_hash == 0x38d65b32`) | same regression |

All three revert cleanly; the guard as written is load-bearing and stays.

## What that means

The redirect is not just a sampling choice: it is what makes the upscaler's output the image the UI
composite and the display path agree on. Bypassing it for a chain image desynchronises that chain
and the frame falls back to the render size in the corner of the output.

So the light cannot be carried by reading the proxy *instead* of the redirected image. It has to be
produced where the display path already reads:

- **structural**: reorder the chain the way the reference does it — upscale the scene first, then
  run the composite at output size. Then the extended composite writes into the image the display
  copy samples, and no redirect is involved. This is the real fix and it is a chain-level change,
  not a shader-level one.
- **pragmatic**: apply the extension in bbport's own display-copy shader, at output size, using the
  chain's SDR value. That reaches the panel sooner but needs the pre-clamp scene value, i.e. an
  estimate again — the very thing the IR extension was adopted to avoid.

Neither is a small edit, and the second abandons the mechanism this port is based on.

## State

Nothing of the three attempts is left in the tree. The extension still runs behind
`BB_HDR_SCENE_CURVE` (default off), the composite is extended correctly, and the panel stays at
paper white, so step 4 of the objective — retiring `bb_composite_hdr.frag`, `bb_tonemap_variants.h`,
the delta pipeline and `hdr_delta` — is **not** reached: the delta machinery is still the only path
that has ever put light above white on the screen, and it must not be removed while the IR path
cannot transport its output.

Still to fix before any of this ships: the shader cache holds the extended composite, so
`BB_HDR_SCENE_CURVE=0` cannot disable the feature until the cache is separated the way the reference
does it (`cache_storage.cpp`).
