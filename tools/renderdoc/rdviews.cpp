// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The views a texture is written and read through: formats, view types, the texture's flags.
#include <cstdio>
#include <cstdlib>
#include "renderdoc_replay.h"
template <>
rdcstr DoStringise(const uint32_t &el) { char b[16]; snprintf(b, sizeof(b), "%u", el); return b; }
#include "pipestate.inl"
REPLAY_PROGRAM_MARKER()
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "rdviews capture.rdc event...\n"); return 2; }
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
  for (int i = 2; i < argc; ++i) {
    const uint32_t eid = atoi(argv[i]);
    ctrl->SetFrameEvent(eid, true);
    const PipeState &ps = ctrl->GetPipelineState();
    for (ShaderStage st : {ShaderStage::Compute, ShaderStage::Fragment})
      for (int rw = 0; rw < 2; ++rw)
        for (const UsedDescriptor &u : rw ? ps.GetReadWriteResources(st, true) : ps.GetReadOnlyResources(st, true)) {
          for (const TextureDescription &t : ctrl->GetTextures()) {
            if (t.resourceId != u.descriptor.resource || t.width != 1025) continue;
            printf("event %u %s %s: view format %s type %u; texture format %s flags %#x\n", eid,
                   st == ShaderStage::Compute ? "cs" : "ps", rw ? "rw" : "ro", u.descriptor.format.Name().c_str(),
                   (uint32_t)u.descriptor.textureType, t.format.Name().c_str(), (uint32_t)t.creationFlags);
          }
        }
  }
  fflush(stdout);
  remote->CloseCapture(ctrl);
  remote->ShutdownServerAndConnection();
  return 0;
}
