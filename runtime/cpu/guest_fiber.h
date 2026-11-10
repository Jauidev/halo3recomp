#pragma once
// XAPI fibers (ConvertThreadToFiber / CreateFiber / SwitchToFiber / DeleteFiber) on host fibers.
// The console's SwitchToFiber saves the nonvolatile registers in the current fiber's block, loads
// the target's r1 and LR and returns into the target: recompiled code cannot do that (the C++ call
// stack is the real stack). So every guest fiber gets a host fiber with its own host stack and its
// own PPCContext, and a guest switch is a host fiber switch: each fiber's registers simply stay in
// its own context, and each one resumes where its host frames left off.
//
// The title's own XAPI code still builds and frees its fiber blocks (statically linked, so its
// addresses are per title): the title's hooks (games/<game>/game.cpp) call these around it.

struct PPCContext;

// After the title's ConvertThreadToFiber made `fiber` the calling thread's block.
void GuestFiberConvertThread(PPCContext& ctx, uint32_t fiber);

// After the title's CreateFiber built `fiber`. The first switch to it runs entry(ctx) on a fresh
// host fiber with a copy of the switcher's context, r1 = stackPointer and r3 = r31 = argument
// (the XAPI start thunk's convention).
void GuestFiberCreate(uint32_t fiber, PPCFunc* entry, uint32_t stackPointer, uint32_t argument);

// The title's SwitchToFiber: the caller has already stored the target as the current fiber in
// the TEB. Returns when something switches back to the calling fiber.
void GuestFiberSwitch(PPCContext& ctx, uint32_t target);

// Before the title's DeleteFiber / ConvertFiberToThread frees `fiber`'s block.
void GuestFiberDelete(uint32_t fiber);
