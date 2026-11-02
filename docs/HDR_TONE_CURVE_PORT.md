# Native+ HDR: porting the reference's tone-curve extension (state, 2026-11-02)

Where this stands, what has been ruled out, and what to do next. Written after
the delta-shader/alpha-carrier road was abandoned.

## The mechanism being ported

The reference (`LanternEngine`, `shader_recompiler/native/tone_curve.{h,cpp}`)
does not replace the composite shader and has no side channel. A compiler pass
finds the game's own tone curve in the composite's IR and extends it above a
knee, so the composite itself writes past 1.0:

- exponential: `y = A * B * B`, `A = 1 - exp2(-1.442695 * k * x)`,
  `B = 1 + c * (A - 1)`, k/c in constant-buffer words 832/833;
- extended Reinhard: `y = v * (1 + w * v) / (1 + v)`, `v = k * x`, words 833/834;
- each anchored at the `FPSaturate32` clamp to [0, 1]; `IsComposite` wants three
  channels, one family, one constant set, three scene values;
- `ExtendToneCurve` replaces each curve with a select: the game's own
  instructions bit-for-bit below a knee, an extension above it that is
  continuous in value and slope and rolls off to peak / paper white
  (`ToneCurveRollOff` 70% of the stops linear), and the clamps after the curve
  stop at the peak instead of 1;
- parameters ride in push data as `ToneCurvePush { knee, peak, strength, enabled }`.

## Done, on branch `hdr-nativeplus-port`

- `69ed473`, `881b719` — the 16-bit chain transport: chain targets get
  same-size RGBA16F scene proxies, pipelines re-keyed to the float attachment
  format, display buffers keep their own redirect. **Verified**: picture full
  width, ReShade panel max 202.9121 nits (paper white), highlights intact.
- `eb19619` — the whole 434-line pass compiles in bbport's IR. Only the include
  paths changed (`shader_recompiler/` → `shader/`); the file lives at
  `gpu/shader/native/tone_curve.{h,cpp}` and the `GLOB_RECURSE` picks it up.
  Added: `OPCODE(ToneCurveParam, F32, U32)`, `IREmitter::ToneCurveParam`,
  `EmitToneCurveParam` (access chain into the push block at
  `PushData::ToneCurveIndex`), and `PushData::tone_curve[4]`.
- `52024cf`, `35d1694` — the pass is hooked into `pipeline_compile.cpp` behind
  `BB_HDR_SCENE_CURVE` and logs every fresh fragment translation with its hash
  and curve count.
- `db25eae` — the hook is **recognition-only**: `ExtendToneCurve` is not called
  yet (see below).

## Ruled out (do not redo)

- Delta shader / alpha carrier: forcing the alpha to a constant 0.5 left the
  `BB_HDR_DEBUG=1` probe at 81% white / 0% blue, so that draw's output never
  reaches the image the present samples. Three rounds, abandoned.
- `FPSaturate32` missing from bbport's IR: it exists — `translate.cpp` saturates
  go through `ir.FPSaturate(...)` and `ir_emitter.cpp:897`
  `BB_FP_UNARY(FPSaturate, FPSaturate)` maps to `Opcode::FPSaturate32`.
- Exp shape: both frontends emit a plain `FPExp2` (bbport `vector_alu.cpp:325`,
  engine `vector_alu.cpp:1032`).
- `ReadConstBuffer` absent under native shaders: it is emitted unconditionally at
  `frontend/translate/scalar_memory.cpp:85`; `BB_NATIVE_SHADERS` is a backend
  lowering, not an IR change.
- Offset 128 for the push block: bbport's `bb_hdr` already owns it. The ported
  parameters live at **176** (`ToneCurveIndex = BbHdr2Index + 8`), struct now
  192 bytes with its `static_assert` updated.

## Where the last measurement went wrong

The hook reported 0 curves on every run — but in all but one of those the shader
came from the SPIR-V cache, so the pass saw no fresh translation at all. The one
run that could have matched was on a cold cache and crashed during recompiles,
and the cache had been deleted by us (74 MB). So **the "no match" result carries
no evidence**; the matcher may well work.

Lesson (now the working rule): a symptom counts only if it survives three runs,
and after three identical failures go back to the reference source instead of
experimenting further.

## Next steps, in order

1. **Recognition, safely.** The hook no longer calls `ExtendToneCurve`, so this
   costs nothing and cannot crash:
   ```
   $env:BB_HDR_SCENE_CURVE='1'; & scripts\verify-hdr-fast.ps1 -DataRoot 'E:\Bloodborne-bbport-nativeplus-hdr'
   ```
   Look for `HDR tone curve: fragment 5e67d866, 3 curve(s)`. Run it up to three
   times (Rule 1). Do not delete the shader cache to force recompiles; any new
   program logs its own line.
2. **Producer side** (needed before the extension can be correct):
   - grow the push range to 192 where `GraphicsPipeline` sizes it — the
     reference's line is `std::max(HdrChannel::PushConstantSize(), tone_curve_size)`;
   - push `{knee, peak, strength, enabled}` per draw from `hdr_knee`,
     `PeakNits()/paper_white`, the strength slider and the switch;
   - add `Shader::Info::uses_tone_curve` (bbport has no such flag; the ported
     pass has that assignment commented out) so a pipeline knows it must push.
3. **Then** re-enable `ExtendToneCurve` and measure the panel: the composite's
   own target is already float, so above-white should reach the present.
4. Only after that: retire `bb_composite_hdr.frag`, `bb_tonemap_variants.h`, the
   delta pipeline and `hdr_delta`.

## Environment notes

- The shell here is PowerShell 5.1: no `Set-Content -NoNewline`, and `"$nlX"`
  silently parses as an undefined variable — use `[System.IO.File]` APIs or the
  edit tool (which requires a prior read).
- The install under test is `E:\Bloodborne-bbport-nativeplus-hdr`, mirrored from
  `C:\Users\wow99\Downloads\Bloodborne-bbport-0.7-windows-x64`, with the save
  copied from `E:\LanternEngine-NativePlusHDR\user` and `bbport.ini` set to
  `hdr=1 hdr_mode=native+ hdr_peak=790 hdr_paper_white=203`.
- Measurement is the ReShade HDR Analysis panel: crop the top-left 700x240 and
  read max/avg. Screenshots are 3840x2160 and must be downscaled before viewing.

## Session record, 2026-11-02 (late)

Head `12744b7`, twelve commits past `16087ae`. **The local branch is ahead of
the fork** (fork still at `16087ae`), so push before anything else.

Landed after the text above was written:

- `10319bd`, `1e00a7b` â€” the producer side the "next steps" asked for: the push
  block carries the HDR tails (`ToneCurveIndex`, 192 bytes), the curve
  parameters are addressable, and they are pushed per draw.
- `10319bd` also **enables the extension**: the composite's own curve now writes
  past 1.0. The mechanism works; this is the first time the game's own shader is
  the source of above-white in this port.
- `12744b7` â€” the upscaler's input is described as the float image it is.

Measured after enabling: the curve runs, but the frame **still clamps at 1.0**
(ReShade panel max 202.9121 nits). So the light is being lost *after* the
shader, not by it.

Ruled out, do not redo: three attempts to carry the curve through the read path
broke the composition (`a4edffb`), and the tone-curve/transport split was
recorded when the extension first ran (`a65fd34`).

New understanding (`75ebca0`): the chain order is **composite then upscaler**, so
above-white has to survive the *upscaler*. That is the next place to look: the
upscaler's input and output formats and the transfer around it
(`hdr/upscale_transfer.glsl`, which re-encodes to gamma 2.2 between them).

Next steps:

1. Push the branch (it is twelve commits ahead of the fork).
2. With `BB_HDR_SCENE_CURVE=1`, confirm in the log which fragments are extended
   and that the composite's own draw reports a float target
   (`HDR chain draw: ... float target yes`).
3. Find the clamp: composite target -> upscaler input -> upscaler output ->
   display copy -> present. One probe value above white, read at each stage.
4. Only then retire `bb_composite_hdr.frag`, `bb_tonemap_variants.h`, the delta
   pipeline and `hdr_delta`.
