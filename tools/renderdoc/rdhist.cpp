// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Pixel history at one pixel of an event's first colour output, replayed on the Android device.
#include <cstdio>
#include <cstdlib>
#include "renderdoc_replay.h"
template <>
rdcstr DoStringise(const uint32_t &el) {
  char b[16];
  snprintf(b, sizeof(b), "%u", el);
  return b;
}
#include "pipestate.inl"
REPLAY_PROGRAM_MARKER()
static const ActionDescription *Find(const rdcarray<ActionDescription> &list, uint32_t eid) {
  for (const ActionDescription &a : list) {
    if (a.eventId == eid) return &a;
    if (const ActionDescription *c = Find(a.children, eid)) return c;
  }
  return NULL;
}
int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "rdhist capture event x y\n"); return 2; }
  GlobalEnvironment env;
  RENDERDOC_InitialiseReplay(env, {});
  IDeviceProtocolController *adb = RENDERDOC_GetDeviceProtocolController("adb");
  rdcarray<rdcstr> devices = adb->GetDevices();
  if (devices.empty()) { printf("no device\n"); return 1; }
  rdcstr url = adb->GetProtocolName() + "://" + devices[0];
  printf("device %s\n", url.c_str());
  adb->StartRemoteServer(url);
  IRemoteServer *remote = NULL;
  if (!RENDERDOC_CreateRemoteServerConnection(url, &remote).OK() || !remote) { printf("no connection\n"); return 1; }
  rdcstr path = remote->CopyCaptureToRemote(argv[1], NULL);
  rdcpair<ResultDetails, IReplayController *> opened = remote->OpenCapture(~0U, path, ReplayOptions(), NULL);
  if (!opened.first.OK()) { printf("replay: code %d\n", (int)opened.first.code); return 1; }
  IReplayController *ctrl = opened.second;
  const uint32_t eid = atoi(argv[2]);
  const ActionDescription *a = Find(ctrl->GetRootActions(), eid);
  if (!a) { printf("no event %u\n", eid); return 1; }
  ctrl->SetFrameEvent(eid, true);
  {
    const PipeState &ps = ctrl->GetPipelineState();
    for (const UsedDescriptor &u : ps.GetReadOnlyResources(ShaderStage::Fragment, true)) {
      const Descriptor &d = u.descriptor;
      uint32_t ttype = 99, dim = 0, w = 0, h = 0, arr = 0, mips = 0;
      for (const TextureDescription &t : ctrl->GetTextures())
        if (t.resourceId == d.resource) { ttype = (uint32_t)t.type; dim = t.dimension; w = t.width; h = t.height; arr = t.arraysize; mips = t.mips; }
      printf("ro: image type %u dim %u %ux%u array %u mips %u; view type %u slices %u+%u mips %u+%u format %s\n",
             ttype, dim, w, h, arr, mips, (uint32_t)d.textureType, d.firstSlice, d.numSlices, d.firstMip, d.numMips,
             d.format.Name().c_str());
    }
    for (const UsedDescriptor &u : ps.GetSamplers(ShaderStage::Fragment, true)) {
      const SamplerDescriptor &sd = u.sampler;
      printf("sampler: addr %u %u %u filter min %u mag %u mip %u func %u compare %u unnormalized %d lod %g..%g bias %g aniso %g\n",
             (uint32_t)sd.addressU, (uint32_t)sd.addressV, (uint32_t)sd.addressW, (uint32_t)sd.filter.minify,
             (uint32_t)sd.filter.magnify, (uint32_t)sd.filter.mip, (uint32_t)sd.filter.filter,
             (uint32_t)sd.compareFunction, sd.unnormalized, sd.minLOD, sd.maxLOD, sd.mipBias, sd.maxAnisotropy);
    }
  }
  uint32_t px = atoi(argv[3]), py = atoi(argv[4]);
  {
    // the input image's brightest grid point (the title is mostly dark)
    const PipeState &ps = ctrl->GetPipelineState();
    rdcarray<UsedDescriptor> ro = ps.GetReadOnlyResources(ShaderStage::Fragment, true);
    float best = -1;
    if (!ro.empty()) {
      for (uint32_t gy = 0; gy < 27; ++gy) for (uint32_t gx = 0; gx < 48; ++gx) {
        const uint32_t x = gx * 40 + 20, y = gy * 40 + 20;
        PixelValue v = ctrl->PickPixel(ro[0].descriptor.resource, x, y, Subresource(), CompType::Typeless);
        const float sum = v.floatValue[0] + v.floatValue[1] + v.floatValue[2];
        if (sum > best) { best = sum; px = x; py = y; }
      }
      PixelValue in = ctrl->PickPixel(ro[0].descriptor.resource, px, py, Subresource(), CompType::Typeless);
      PixelValue outv = ctrl->PickPixel(a->outputs[0], px, py, Subresource(), CompType::Typeless);
      printf("brightest input at %u,%u: in (%g %g %g %g) out (%g %g %g %g)\n", px, py, in.floatValue[0], in.floatValue[1],
             in.floatValue[2], in.floatValue[3], outv.floatValue[0], outv.floatValue[1], outv.floatValue[2], outv.floatValue[3]);
    }
  }
  {
    const PipeState &ps = ctrl->GetPipelineState();
    rdcarray<UsedDescriptor> ro = ps.GetReadOnlyResources(ShaderStage::Fragment, true);
    for (const UsedDescriptor &u : ro) {
      bool lut = false;
      for (const TextureDescription &t : ctrl->GetTextures())
        if (t.resourceId == u.descriptor.resource && t.width == 1025) lut = true;
      if (!lut) continue;
      const VKPipe::State *vk = ctrl->GetVulkanPipelineState();
      for (const VKPipe::ImageData &im : vk->images)
        if (im.resourceId == u.descriptor.resource)
          for (const VKPipe::ImageLayout &l : im.layouts)
            printf("table layout: mips %u+%u layers %u+%u %s\n", l.baseMip, l.numMip, l.baseLayer, l.numLayer, l.name.c_str());
      for (const VKPipe::ImageData &im : vk->images)
        for (const UsedDescriptor &u2 : ro)
          if (im.resourceId == u2.descriptor.resource && im.resourceId != u.descriptor.resource)
            for (const VKPipe::ImageLayout &l : im.layouts) printf("input image layout: %s\n", l.name.c_str());
      for (uint32_t slice = 0; slice < 3; ++slice) {
        printf("table layer %u:", slice);
        for (uint32_t x : {0u, 256u, 512u, 768u, 1023u, 1024u}) {
          PixelValue v = ctrl->PickPixel(u.descriptor.resource, x, 0, Subresource(0, slice, 0), CompType::Typeless);
          printf(" [%u]=%g", x, v.floatValue[0]);
        }
        printf("\n");
      }
    }
  }
  rdcarray<PixelModification> hist = ctrl->PixelHistory(a->outputs[0], px, py, Subresource(), CompType::Typeless);
  printf("%d modifications at %u,%u\n", (int)hist.count(), px, py);
  for (const PixelModification &m : hist) {
    printf("event %u frag %u prim %u: pre(%g %g %g %g) shader(%g %g %g %g) post(%g %g %g %g)%s%s%s%s%s%s%s%s%s%s%s%s\n",
           m.eventId, m.fragIndex, m.primitiveID, m.preMod.col.floatValue[0], m.preMod.col.floatValue[1],
           m.preMod.col.floatValue[2], m.preMod.col.floatValue[3], m.shaderOut.col.floatValue[0],
           m.shaderOut.col.floatValue[1], m.shaderOut.col.floatValue[2], m.shaderOut.col.floatValue[3],
           m.postMod.col.floatValue[0], m.postMod.col.floatValue[1], m.postMod.col.floatValue[2],
           m.postMod.col.floatValue[3], m.directShaderWrite ? " direct" : "", m.unboundPS ? " unboundPS" : "",
           m.sampleMasked ? " sampleMasked" : "", m.backfaceCulled ? " backfaceCulled" : "",
           m.depthClipped ? " depthClipped" : "", m.viewClipped ? " viewClipped" : "",
           m.scissorClipped ? " scissorClipped" : "", m.shaderDiscarded ? " shaderDiscarded" : "",
           m.depthTestFailed ? " depthTestFailed" : "", m.stencilTestFailed ? " stencilTestFailed" : "",
           m.depthBoundsFailed ? " depthBoundsFailed" : "", m.predicationSkipped ? " predicationSkipped" : "");
  }
  fflush(stdout);
  remote->CloseCapture(ctrl);
  remote->ShutdownServerAndConnection();
  return 0;
}
