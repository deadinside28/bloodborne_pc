// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Replays a RenderDoc capture and prints, for each draw/dispatch/clear/copy, the min/max of its
// first outputs: where a frame goes black.
#include <cstdio>
#include <map>
#include "renderdoc_replay.h"
// pipestate.inl names bind points with this (not exported by the library)
template <>
rdcstr DoStringise(const uint32_t &el) {
  char b[16];
  snprintf(b, sizeof(b), "%u", el);
  return b;
}
#include "pipestate.inl"

REPLAY_PROGRAM_MARKER()

static IReplayController *ctrl;
static std::map<ResourceId, TextureDescription> textures;

static void Walk(const rdcarray<ActionDescription> &actions, int depth) {
  for (const ActionDescription &a : actions) {
    rdcstr name = a.GetName(ctrl->GetStructuredFile());
    printf("%6u %*s%.80s", a.eventId, depth * 2, "", name.c_str());
    const ActionFlags interesting = ActionFlags::Drawcall | ActionFlags::Dispatch | ActionFlags::Clear |
                                    ActionFlags::Copy | ActionFlags::Resolve | ActionFlags::Present;
    if (a.flags & interesting) {
      ctrl->SetFrameEvent(a.eventId, false);
      rdcarray<ResourceId> outs;
      for (ResourceId o : a.outputs)
        if (o != ResourceId()) outs.push_back(o);
      if (a.depthOut != ResourceId()) outs.push_back(a.depthOut);
      if (a.copyDestination != ResourceId()) outs.push_back(a.copyDestination);
      for (int i = 0; i < outs.count() && i < 3; ++i) {
        auto t = textures.find(outs[i]);
        if (t == textures.end()) { printf("  -> (buffer)"); continue; }
        rdcpair<PixelValue, PixelValue> mm = ctrl->GetMinMax(outs[i], Subresource(), CompType::Typeless);
        printf("  -> %ux%u %s min(%.3g %.3g %.3g %.3g) max(%.3g %.3g %.3g %.3g)", t->second.width,
               t->second.height, t->second.format.Name().c_str(), mm.first.floatValue[0],
               mm.first.floatValue[1], mm.first.floatValue[2], mm.first.floatValue[3],
               mm.second.floatValue[0], mm.second.floatValue[1], mm.second.floatValue[2],
               mm.second.floatValue[3]);
      }
    }
    printf("\n");
    if (a.flags & (ActionFlags::Drawcall | ActionFlags::Dispatch)) {
      const PipeState &ps = ctrl->GetPipelineState();
      const ShaderStage stages[] = {ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::Compute};
      for (ShaderStage st : stages) {
        for (int rw = 0; rw < 2; ++rw) {
          rdcarray<UsedDescriptor> used = rw ? ps.GetReadWriteResources(st, true) : ps.GetReadOnlyResources(st, true);
          for (const UsedDescriptor &u : used) {
            ResourceId id = u.descriptor.resource;
            if (id == ResourceId()) continue;
            auto t = textures.find(id);
            printf("%6s %*s  %s %s ", "", depth * 2, "", st == ShaderStage::Vertex ? "vs" : st == ShaderStage::Fragment ? "ps" : "cs", rw ? "rw" : "ro");
            if (t == textures.end()) { printf("buffer, %llu bytes at %llu\n", (unsigned long long)u.descriptor.byteSize, (unsigned long long)u.descriptor.byteOffset); continue; }
            rdcpair<PixelValue, PixelValue> mm = ctrl->GetMinMax(id, Subresource(), CompType::Typeless);
            printf("%ux%u %s min(%.3g %.3g %.3g %.3g) max(%.3g %.3g %.3g %.3g)\n", t->second.width, t->second.height,
                   t->second.format.Name().c_str(), mm.first.floatValue[0], mm.first.floatValue[1], mm.first.floatValue[2],
                   mm.first.floatValue[3], mm.second.floatValue[0], mm.second.floatValue[1], mm.second.floatValue[2], mm.second.floatValue[3]);
          }
        }
      }
    }
    if (!a.children.empty()) Walk(a.children, depth + 1);
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "rdremote capture.rdc\n"); return 2; }
  GlobalEnvironment env;
  RENDERDOC_InitialiseReplay(env, {});
  IDeviceProtocolController *adb = RENDERDOC_GetDeviceProtocolController("adb");
  if (!adb) { printf("no adb protocol\n"); return 1; }
  rdcarray<rdcstr> devices = adb->GetDevices();
  printf("%d adb devices\n", (int)devices.count());
  if (devices.empty()) return 1;
  rdcstr url = adb->GetProtocolName() + "://" + devices[0];
  printf("device %s (%s)\n", url.c_str(), adb->GetFriendlyName(url).c_str());
  ResultDetails started = adb->StartRemoteServer(url);
  printf("start server: code %d\n", (int)started.code);
  IRemoteServer *remote = NULL;
  ResultDetails conn = RENDERDOC_CreateRemoteServerConnection(url, &remote);
  printf("connect: code %d\n", (int)conn.code);
  if (!conn.OK() || !remote) return 1;
  rdcstr remote_path = remote->CopyCaptureToRemote(argv[1], NULL);
  printf("copied to %s\n", remote_path.c_str());
  fflush(stdout);
  rdcpair<ResultDetails, IReplayController *> opened =
      remote->OpenCapture(~0U, remote_path, ReplayOptions(), NULL);
  printf("replay: code %d\n", (int)opened.first.code);
  fflush(stdout);
  if (!opened.first.OK()) { remote->ShutdownServerAndConnection(); return 1; }
  ctrl = opened.second;
  for (const TextureDescription &t : ctrl->GetTextures()) textures[t.resourceId] = t;
  printf("%d textures\n", (int)textures.size());
  Walk(ctrl->GetRootActions(), 0);
  fflush(stdout);
  remote->CloseCapture(ctrl);
  remote->ShutdownServerAndConnection();
  return 0;
}
