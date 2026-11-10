// stand-in for Xenia's Emulator (the GPU only reads the audio system's name for the
// window title; there is no window title on Android).
#ifndef XENIA_EMULATOR_H_
#define XENIA_EMULATOR_H_

#include "xenia/apu/audio_system.h"

namespace xe {

class Emulator {
 public:
  apu::AudioSystem* audio_system() { return nullptr; }
};

}  // namespace xe

#endif  // XENIA_EMULATOR_H_
