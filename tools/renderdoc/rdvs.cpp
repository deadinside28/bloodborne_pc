// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Vertex shader results at an event, replayed on the device: the driver's (post-VS data) beside
// RenderDoc's SPIR-V interpreter on the same vertices. Differences point at the driver's compiler.
// rdvs capture.rdc            lists the draws
// rdvs capture.rdc event [n]  compares n vertices spread over the draw (default 12)
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
static void List(const rdcarray<ActionDescription> &list) {
  for (const ActionDescription &a : list) {
    if (a.flags & ActionFlags::Drawcall) {
      int outs = 0;
      for (ResourceId o : a.outputs) outs += o != ResourceId();
      printf("event %u: %u indices, %u instances, %d colour outputs\n", a.eventId, a.numIndices, a.numInstances, outs);
    }
    List(a.children);
  }
}
int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "rdvs capture.rdc [event [n]]\n"); return 2; }
  GlobalEnvironment env;
  RENDERDOC_InitialiseReplay(env, {});
  IDeviceProtocolController *adb = RENDERDOC_GetDeviceProtocolController("adb");
  rdcarray<rdcstr> devices = adb->GetDevices();
  if (devices.empty()) { printf("no device\n"); return 1; }
  rdcstr url = adb->GetProtocolName() + "://" + devices[0];
  adb->StartRemoteServer(url);
  IRemoteServer *remote = NULL;
  if (!RENDERDOC_CreateRemoteServerConnection(url, &remote).OK() || !remote) { printf("no connection\n"); return 1; }
  rdcstr path = remote->CopyCaptureToRemote(argv[1], NULL);
  rdcpair<ResultDetails, IReplayController *> opened = remote->OpenCapture(~0U, path, ReplayOptions(), NULL);
  if (!opened.first.OK()) { printf("replay: code %d\n", (int)opened.first.code); return 1; }
  IReplayController *ctrl = opened.second;
  if (argc < 3) List(ctrl->GetRootActions());
  else {
    const uint32_t eid = atoi(argv[2]);
    const uint32_t n = argc > 3 ? atoi(argv[3]) : 12;
    const ActionDescription *a = Find(ctrl->GetRootActions(), eid);
    if (!a) { printf("no event\n"); return 1; }
    ctrl->SetFrameEvent(eid, true);
    const PipeState &ps = ctrl->GetPipelineState();
    MeshFormat m = ctrl->GetPostVSData(0, 0, MeshDataStage::VSOut);
    printf("post-VS: %u indices, stride %llu, status '%s'\n", m.numIndices, (unsigned long long)m.vertexByteStride, m.status.c_str());
    BoundVBuffer ib = ps.GetIBuffer();
    bytebuf ibd = ctrl->GetBufferData(ib.resourceId, ib.byteOffset + a->indexOffset * ib.byteStride, a->numIndices * ib.byteStride);
    bytebuf pib = m.indexResourceId != ResourceId() ? ctrl->GetBufferData(m.indexResourceId, m.indexByteOffset, m.numIndices * m.indexByteStride) : bytebuf();
    bytebuf vb = ctrl->GetBufferData(m.vertexResourceId, m.vertexByteOffset, 0);
    printf("index buffers: original %u bytes stride %u, post %u bytes stride %u; vertices %u bytes\n", (unsigned)ibd.size(),
           ib.byteStride, (unsigned)pib.size(), m.indexByteStride, (unsigned)vb.size());
    if (argc > 4) {
      if (FILE *f = fopen(argv[4], "wb")) { fwrite(vb.data(), 1, vb.size(), f); fclose(f); }
      char ipath[512]; snprintf(ipath, sizeof(ipath), "%s.idx", argv[4]);
      if (FILE *f = fopen(ipath, "wb")) { fwrite(pib.data(), 1, pib.size(), f); fclose(f); }
    }
    const auto rd = [](const bytebuf &b, uint32_t i, uint32_t w) { uint32_t v = 0; memcpy(&v, b.data() + i * w, w); return v; };
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t i = uint64_t(k) * a->numIndices / n;
      const uint32_t orig = rd(ibd, i, ib.byteStride) + a->baseVertex;
      const uint32_t r = pib.empty() ? i : rd(pib, i, m.indexByteStride);
      float p[4] = {};
      if ((r + 1) * m.vertexByteStride <= vb.size()) memcpy(p, vb.data() + r * m.vertexByteStride, 16);
      printf("index %u vertex %u: driver %.5f %.5f %.5f %.5f |", i, orig, p[0], p[1], p[2], p[3]);
      ShaderDebugTrace *t = ctrl->DebugVertex(orig, 0, orig, 0);
      if (!t || !t->debugger) { printf(" no trace\n"); continue; }
      float o[4] = {}; bool got = false;
      for (int guard = 0; guard < 100000; ++guard) {
        rdcarray<ShaderDebugState> st = ctrl->ContinueDebug(t->debugger);
        if (st.empty()) break;
        for (const ShaderDebugState &s : st)
          for (const ShaderVariableChange &c : s.changes) {
            if (k == 0 && guard < 100000 && (strstr(c.after.name.c_str(), "ut") || strstr(c.after.name.c_str(), "osition")))
              printf("\n   change %s (%u columns) %.5f %.5f %.5f %.5f", c.after.name.c_str(), (unsigned)c.after.columns,
                     c.after.value.f32v[0], c.after.value.f32v[1], c.after.value.f32v[2], c.after.value.f32v[3]);
            if (strstr(c.after.name.c_str(), "Position") && c.after.columns >= 1) {
              if (c.after.columns == 4) { for (int j = 0; j < 4; ++j) o[j] = c.after.value.f32v[j]; got = true; }
              else if (const char *d = strrchr(c.after.name.c_str(), '.')) { const char *q = strchr("xyzw", d[1]); if (q && d[1]) { o[q - "xyzw"] = c.after.value.f32v[0]; got = true; } }
            }
          }
      }
      ctrl->FreeTrace(t);
      printf(" interpreter %s %.5f %.5f %.5f %.5f | delta %.5f %.5f %.5f\n", got ? "" : "(none)", o[0], o[1], o[2], o[3], p[0] - o[0], p[1] - o[1], p[2] - o[2]);
      fflush(stdout);
    }
  }
  fflush(stdout);
  remote->CloseCapture(ctrl);
  remote->ShutdownServerAndConnection();
  return 0;
}
