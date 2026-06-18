#pragma once
#include <cstdint>

struct IDXGISwapChain;

// Phase 2a frame generation (native D3D11, optical-flow comes in 2b).
// Captures consecutive frames and injects one interpolated frame per real frame
// into the present loop. Interpolation is a placeholder blend for now -- the goal
// of 2a is to prove frame INJECTION works, measured by the generated-frame counter.
namespace framegen {

// Matches IDXGISwapChain::Present (the MinHook trampoline), so we can present an
// extra frame WITHOUT going back through our own hook (which would recurse).
using PresentTrampoline = long (__stdcall*)(IDXGISwapChain*, unsigned, unsigned);

// Called from the present hook BEFORE the real frame is presented.
void before_present(IDXGISwapChain* sc, PresentTrampoline present, unsigned flags);

void on_resize();
void shutdown();

uint64_t real_frames();
uint64_t generated_frames();

} // namespace framegen
