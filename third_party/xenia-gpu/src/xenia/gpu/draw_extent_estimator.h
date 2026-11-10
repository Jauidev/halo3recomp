/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
#define XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_

#include <array>
#include <cstdint>
#include <optional>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const RegisterFile& register_file, const Memory& memory,
                      TraceWriter* trace_writer)
      : register_file_(register_file),
        memory_(memory),
        trace_writer_(trace_writer),
        shader_interpreter_(register_file, memory) {
    shader_interpreter_.SetTraceWriter(trace_writer);
  }

  // The shader must have its ucode analyzed.
  uint32_t EstimateVertexMaxY(const Shader& vertex_shader,
                              bool window_offset_in_edram);
  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y,
                        const Shader& vertex_shader,
                        bool window_offset_in_edram);

  // the pixels covered by the single rectangle drawn by a
  // rectangle list of 3 vertices (how Direct3D 9 draws clears), from the
  // vertex shader run on the CPU, before the scissor - by their centers, or,
  // with whole_pixels (for multisampling), only those wholly inside. False if
  // unknown.
  // z_out, if not null, receives the depth of the rectangle if all its
  // vertices have the same.
  bool XportGetSingleRectangle(const Shader& vertex_shader, bool whole_pixels,
                              uint32_t& left, uint32_t& top, uint32_t& right,
                              uint32_t& bottom,
                              std::optional<float>* z_out = nullptr);
  // the colors that the pixel shader writes to the color targets
  // (all 4 components) if they don't depend on the inputs, like the constant
  // color of a clear. False if the shader can't be run on the CPU.
  bool XportGetConstantPixelColors(
      const Shader& pixel_shader,
      std::optional<std::array<float, 4>> (&colors_out)[4]);

 private:
  class XportExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;
    void Reset() {
      position_mask = 0;
      std::fill(color_masks, color_masks + 4, 0u);
      killed = false;
    }
    float position[4] = {};
    uint32_t position_mask = 0;
    float colors[4][4] = {};
    uint32_t color_masks[4] = {};
    bool killed = false;
  };

  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<float>& position_y() const { return position_y_; }
    const std::optional<float>& position_w() const { return position_w_; }
    const std::optional<float>& point_size() const { return point_size_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<float> position_y_;
    std::optional<float> position_w_;
    std::optional<float> point_size_;
    std::optional<uint32_t> vertex_kill_;
  };

  const RegisterFile& register_file_;
  const Memory& memory_;
  TraceWriter* trace_writer_;

  ShaderInterpreter shader_interpreter_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
