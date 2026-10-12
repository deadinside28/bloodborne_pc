# Shader patches

`BB_PATCH_SHADERS=1` replaces a translated shader by a SPIR-V file of the same interface:
`user/shader/patch/<stage>_0x<hash>_<perm>.spv` (`fs`, `vs`, `cs`; the hash as the frame analyzer
and the log print it; permutation 0 unless the log says otherwise). The original translation is in
`user/cache/<title>/0x<hash>_<perm>.spv`; `spirv-cross --vulkan-semantics` turns it into readable
GLSL with the bindings a replacement must keep. A patch applies at startup, also to pipelines
restored from the pipeline cache, and while the game runs: a patch file written (atomically:
write elsewhere, then rename) is picked up within a second, the pipelines using the shader are
rebuilt. The log says `GPU: shader patch loaded` / `reloaded`.

The launcher's *Experimental shaders* group makes this a setting: a shaders folder holds one
folder per **shader pack** with such `.spv` files (and, by convention, their sources beside
them); each pack has a switch, enabled packs are copied into `<user>/shader/patch` at launch and
`BB_PATCH_SHADERS=1` is set. Files put into the patch folder by hand are not touched.
