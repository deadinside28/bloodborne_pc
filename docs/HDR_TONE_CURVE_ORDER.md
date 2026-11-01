# Native+ tone curve: the chain order, and where the light actually dies

Written after reading the pass order out of a run's log. Supersedes the guesswork in
`HDR_TONE_CURVE_READPATH.md` about which stage is responsible.

## The order, from the log

```
HDR 16-bit: target 0x104ff30000 (from ps 5e67d866) is RGBA16F
HDR chain draw: ps 5e67d866 -> 0x104ff30000 2560x1472 ... float target yes
HDR upscaler: the chain's frame goes to DLSS as linear BT.2020 (2560x1440), its result back to
              gamma 2.2 (3840x2160)
Display: host buffer 3840x2160 (live)
```

So in bbport the frame goes:

    composite (render size, float proxy) -> upscaler (render -> output) -> display image -> present

The composite's extended output is the upscaler's **input**, and the presenter takes the display
image as final (`upscaler.DisplayOverride`), so no second upscale happens after it.

## What that means

- Reading the chain proxy in the display copy cannot work, and the bordered frames from the three
  attempts have a mundane explanation: bypassing the redirect makes that pass sample a render-size
  image while the presenter then treats the result as the final output-size frame. The redirect
  exists precisely to put the upscaler's output there.
- The light therefore has to survive the **upscaler stage**, not the read path. Measured: it does
  not — the panel's maximum is exactly paper white with DLSS *and* with TAA, so it is not the NVNGX
  clamp; it is the stage around it (the transfer, or the upscaler's own target format).
- The reference does not have this problem because its composite runs **after** upscaling, at output
  size, so nothing reconstructs its output. bbport composites **before**.

## So the remaining choice is a chain decision, not an edit

1. **Follow the reference**: composite at output size, after the upscaler. In bbport that means the
   composite's draw has to target an output-size image instead of the game's render-size one — a
   redirect of the composite target, with the shader's coordinate basis adjusted. Large, but it is
   the shape the reference proves works.
2. **Instrument the upscaler stage** before doing anything else: log the format and the maximum of
   the upscaler's input and output once (the transfer writes to the upscaler's image). If the input
   carries >1 and the output does not, the transfer or the upscaler's target format is the fix and
   it is small. This is the cheaper question and should be answered first.

## State

Everything before step 3 of the objective is done and committed: the matcher, the producer side, the
push-block fix, the extension running with `float target yes`, and the scene responding to
`hdr_knee`. Step 3 (above paper white on the panel) is **not** reached; step 4 (retiring the delta
machinery) stays deliberately undone, because that machinery remains the only path that has ever put
light above white on this screen.

Also still open: the shader cache holds the extended composite, so `BB_HDR_SCENE_CURVE=0` cannot
disable the feature until the cache is separated the way the reference does it.
