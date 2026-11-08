// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The display pass of a capture replayed on the Android device: its fragment shader's disassembly
// and a debug trace of one pixel (every variable change).
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

static void Print(const ShaderVariable &v) {
  printf("%s =", v.name.c_str());
  const unsigned n = v.rows * v.columns ? v.rows * v.columns : 1;
  for (unsigned i = 0; i < n && i < 4; ++i)
    printf(" %g(%#x)", v.value.f32v[i], v.value.u32v[i]);
  if (!v.members.empty()) printf(" {%d members}", (int)v.members.count());
  printf("\n");
}

static const ActionDescription *Find(const rdcarray<ActionDescription> &list, uint32_t eid) {
  for (const ActionDescription &a : list) {
    if (a.eventId == eid) return &a;
    if (const ActionDescription *c = Find(a.children, eid)) return c;
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "rddebug capture.rdc event x y\n"); return 2; }
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
  ctrl->SetFrameEvent(atoi(argv[2]), true);
  const PipeState &ps = ctrl->GetPipelineState();
  for (uint32_t i = 0; i < 2; ++i) {
    Viewport vp = ps.GetViewport(i);
    Scissor sc = ps.GetScissor(i);
    printf("viewport %u: enabled %d %g,%g %gx%g depth %g..%g; scissor %d,%d %dx%d\n", i, vp.enabled, vp.x, vp.y,
           vp.width, vp.height, vp.minDepth, vp.maxDepth, sc.x, sc.y, sc.width, sc.height);
  }
  {
    const VKPipe::State *vk = ctrl->GetVulkanPipelineState();
    const ActionDescription *act = Find(ctrl->GetRootActions(), atoi(argv[2]));
    printf("draw: %u indices/vertices, %u instances, vertex offset %d, flags %#x\n", act ? act->numIndices : 0,
           act ? act->numInstances : 0, act ? act->vertexOffset : 0, act ? (unsigned)act->flags : 0);
    printf("topology %d restart %d; discard %d cull %d frontCCW %d fill %d depthClamp %d depthClip %d\n",
           (int)vk->inputAssembly.topology, vk->inputAssembly.primitiveRestartEnable,
           vk->rasterizer.rasterizerDiscardEnable, (int)vk->rasterizer.cullMode, vk->rasterizer.frontCCW,
           (int)vk->rasterizer.fillMode, vk->rasterizer.depthClampEnable, vk->rasterizer.depthClipEnable);
    printf("stages: vs %d tcs %d tes %d gs %d fs %d\n", vk->vertexShader.resourceId != ResourceId(),
           vk->tessControlShader.resourceId != ResourceId(), vk->tessEvalShader.resourceId != ResourceId(),
           vk->geometryShader.resourceId != ResourceId(), vk->fragmentShader.resourceId != ResourceId());
  }
  for (ShaderStage st : {ShaderStage::Tess_Control, ShaderStage::Tess_Eval}) {
    const ShaderReflection *r = ps.GetShaderReflection(st);
    printf("==== %s disassembly\n%s\n", st == ShaderStage::Tess_Control ? "tess control" : "tess eval",
           r ? ctrl->DisassembleShader(ps.GetGraphicsPipelineObject(), r, "").c_str() : "(none)");
  }
  const ShaderReflection *vrefl = ps.GetShaderReflection(ShaderStage::Vertex);
  for (uint32_t b = 0; vrefl && b < vrefl->constantBlocks.count(); ++b) {
    printf("==== vertex constants %s\n", vrefl->constantBlocks[b].name.c_str());
    rdcarray<ShaderVariable> vars = ctrl->GetCBufferVariableContents(
        ps.GetGraphicsPipelineObject(), vrefl->resourceId, ShaderStage::Vertex, vrefl->entryPoint, b, ResourceId(), 0, 0);
    for (const ShaderVariable &v : vars) Print(v);
  }

  for (uint32_t v = 0; v < 3; ++v) {
    ShaderDebugTrace *vt = ctrl->DebugVertex(v, 0, v, 0);
    printf("==== vertex %u trace %s\n", v, vt && vt->debugger ? "" : "(none)");
    if (vt && vt->debugger) {
      for (int guard = 0; guard < 500; ++guard) {
        rdcarray<ShaderDebugState> states = ctrl->ContinueDebug(vt->debugger);
        if (states.empty()) break;
        for (const ShaderDebugState &st : states)
          for (const ShaderVariableChange &c : st.changes) { printf("  step %u: ", st.stepIndex); Print(c.after); }
      }
      ctrl->FreeTrace(vt);
    }
  }
  const ShaderReflection *refl = ps.GetShaderReflection(ShaderStage::Fragment);
  
  ShaderDebugTrace *trace = ctrl->DebugPixel(atoi(argv[3]), atoi(argv[4]), DebugPixelInputs());
  if (!trace || !trace->debugger) { printf("no debug trace\n"); }
  else {
    printf("==== inputs\n");
    for (const ShaderVariable &v : trace->inputs) Print(v);
    printf("==== constants\n");
    for (const ShaderVariable &v : trace->constantBlocks) { Print(v); for (const ShaderVariable &m : v.members) { printf("   "); Print(m); } }
    printf("==== steps\n");
    for (int guard = 0; guard < 2000; ++guard) {
      rdcarray<ShaderDebugState> states = ctrl->ContinueDebug(trace->debugger);
      if (states.empty()) break;
      for (const ShaderDebugState &s : states)
        for (const ShaderVariableChange &c : s.changes) { printf("step %u inst %u: ", s.stepIndex, s.nextInstruction); Print(c.after); }
    }
    ctrl->FreeTrace(trace);
  }
  fflush(stdout);
  remote->CloseCapture(ctrl);
  remote->ShutdownServerAndConnection();
  return 0;
}
