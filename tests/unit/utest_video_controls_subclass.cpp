/// 测试类型: unit
/// 目标单元: include/aurora/aurora.h
/// 测试说明: video_controls_subclass 单元测试
///

#include "aurora/aurora.h"
#include "aurora/media/video_controls.h"

#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_video_controls_subclass {



namespace {
// #3: 子类化 VideoControls 换肤/重排；build_children() 覆写生效
class ThemedControls : public au::VideoControls {
  public:
    explicit ThemedControls(au::VideoController *c) : VideoControls(c) {}
    auto init() -> void { build_children(); } // 构造后调用，避免构造体内虚函数调用
    auto build_called() const -> bool { return m_build_called_; }
    auto probe_play() const -> au::Button * { return play_button(); }
    auto probe_time() const -> au::Text * { return time_text(); }
    auto probe_mute() const -> au::Button * { return mute_button(); }

  protected:
    auto build_children() -> void override {
        VideoControls::build_children();
        m_build_called_ = true;
    }

  private:
    bool m_build_called_ = false;
};
} // namespace

AURORA_TEST() {
    ThemedControls tc{ nullptr };
    tc.init();
    AURORA_TEST_CHECK(tc.probe_play() != nullptr); // #3: play_button() 受保护访问器
    AURORA_TEST_CHECK(tc.probe_time() != nullptr); // #3: time_text() 受保护访问器
    AURORA_TEST_CHECK(tc.probe_mute() != nullptr); // #3: mute_button() 受保护访问器
    AURORA_TEST_CHECK(tc.build_called());          // #3: build_children() 虚函数覆写生效
}


}  // namespace aurora::test_cases::utest_video_controls_subclass