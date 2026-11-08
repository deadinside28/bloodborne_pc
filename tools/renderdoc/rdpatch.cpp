// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// rdpatch capture event x y dump.spv [variant.spv ...]: the event's fragment shader written to
// dump.spv, then each variant swapped in and the output pixel read back after the event.
#include <cstdio>
#include <cstdlib>
#include "renderdoc_replay.h"
template <>
rdcstr DoStringise(const uint32_t &el) { char b[16]; snprintf(b, sizeof(b), "%u", el); return b; }
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
  if (argc < 6) { fprintf(stderr, "rdpatch capture.rdc event x y dump.spv [variant.spv ...]\n"); return 2; }
  GlobalEnvironment env;
  RENDERDOC_InitialiseReplay(env, {});
  IDeviceProtocolController *adb = RENDERDOC_GetDeviceProtocolController("adb");
  rdcarray<rdcstr> devices = adb->GetDevices();
  if (devices.empty()) return 1;
  rdcstr url = adb->GetProtocolName() + "://" + devices[0];
  adb->StartRemoteServer(url);
  IRemoteServer *remote = NULL;
  if (!RENDERDOC_CreateRemoteServerConnection(url, &remote).OK() || !remote) return 1;
  rdcstr path = remote->CopyCaptureToRemote(argv[1], NULL);
  rdcpair<ResultDetails, IReplayController *> opened = remote->OpenCapture(~0U, path, ReplayOptions(), NULL);
  if (!opened.first.OK()) { printf("replay: code %d\n", (int)opened.first.code); return 1; }
  IReplayController *ctrl = opened.second;
  const uint32_t eid = atoi(argv[2]), x = atoi(argv[3]), y = atoi(argv[4]);
  const ActionDescription *a = Find(ctrl->GetRootActions(), eid);
  ctrl->SetFrameEvent(eid, true);
  const ShaderReflection *refl = ctrl->GetPipelineState().GetShaderReflection(ShaderStage::Fragment);
  const ResourceId original = refl->resourceId;
  const rdcstr entry = refl->entryPoint;
  if (FILE *f = fopen(argv[5], "wb")) { fwrite(refl->rawBytes.data(), 1, refl->rawBytes.size(), f); fclose(f); }
  PixelValue v = ctrl->PickPixel(a->outputs[0], x, y, Subresource(), CompType::Typeless);
  printf("original: (%g %g %g %g)\n", v.floatValue[0], v.floatValue[1], v.floatValue[2], v.floatValue[3]);
  for (int i = 6; i < argc; ++i) {
    FILE *f = fopen(argv[i], "rb");
    if (!f) continue;
    bytebuf code;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) code.append(buf, n);
    fclose(f);
    rdcpair<ResourceId, rdcstr> built = ctrl->BuildTargetShader(entry, ShaderEncoding::SPIRV, code, ShaderCompileFlags(), ShaderStage::Fragment);
    if (built.first == ResourceId()) { printf("%s: build failed: %s\n", argv[i], built.second.c_str()); continue; }
    ctrl->ReplaceResource(original, built.first);
    ctrl->SetFrameEvent(eid, true);
    v = ctrl->PickPixel(a->outputs[0], x, y, Subresource(), CompType::Typeless);
    printf("%s: (%g %g %g %g)\n", argv[i], v.floatValue[0], v.floatValue[1], v.floatValue[2], v.floatValue[3]);
    ctrl->RemoveReplacement(original);
    ctrl->FreeTargetResource(built.first);
  }
  fflush(stdout);
  remote->CloseCapture(ctrl);
  remote->ShutdownServerAndConnection();
  return 0;
}
