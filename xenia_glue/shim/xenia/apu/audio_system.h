// stand-in for Xenia's AudioSystem (only its name is read, and never here).
#ifndef XENIA_APU_AUDIO_SYSTEM_H_
#define XENIA_APU_AUDIO_SYSTEM_H_

#include <string>

namespace xe {
namespace apu {

class AudioSystem {
 public:
  std::string name() const { return "xport"; }
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_AUDIO_SYSTEM_H_
