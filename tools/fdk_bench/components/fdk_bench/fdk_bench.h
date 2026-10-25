#pragma once
#include "esphome/core/component.h"

namespace esphome {
namespace fdk_bench {

class FdkBench : public Component {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
};

}  // namespace fdk_bench
}  // namespace esphome
