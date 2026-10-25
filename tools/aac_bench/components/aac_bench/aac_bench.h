#pragma once
#include "esphome/core/component.h"

namespace esphome {
namespace aac_bench {

class AacBench : public Component {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
};

}  // namespace aac_bench
}  // namespace esphome
