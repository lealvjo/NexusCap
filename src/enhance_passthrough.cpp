#include "nexuscap/enhance.hpp"

namespace nexuscap {
namespace {

class PassthroughEnhance final : public IFrameEnhance {
public:
  bool init(ID3D11Device*, std::string& err) override {
    (void)err;
    inited_ = true;
    return true;
  }
  void shutdown() override { inited_ = false; }
  void set_options(const EnhanceOptions& opt) override { opt_ = opt; }
  EnhanceOptions options() const override { return opt_; }
  bool feature_supported() const override { return false; }
  bool enabled() const override { return false; }
  const char* backend_name() const override { return "passthrough"; }

  bool evaluate(const EnhanceFrameIn& in, EnhanceFrameOut& out, std::string& err) override {
    (void)err;
    out.color_out = in.color_rgb;
    out.dlss_ms = 0;
    out.evaluated = false;
    out.status = "passthrough";
    return true;
  }

private:
  bool inited_ = false;
  EnhanceOptions opt_{};
};

}  // namespace

std::unique_ptr<IFrameEnhance> create_passthrough_enhance() {
  return std::make_unique<PassthroughEnhance>();
}

}  // namespace nexuscap
