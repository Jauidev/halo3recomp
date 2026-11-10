// stand-in for Xenia's XmaDecoder header. xma_context.cc includes it but uses nothing
// from it; the real one needs Xenia's CPU (cpu::Processor). The runtime drives the contexts
// through xenia_glue/apu.cc instead.
#ifndef XENIA_APU_XMA_DECODER_H_
#define XENIA_APU_XMA_DECODER_H_

#include "xenia/apu/xma_context.h"

#endif  // XENIA_APU_XMA_DECODER_H_
