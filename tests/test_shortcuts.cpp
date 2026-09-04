// 验证快捷键绑定：KeyCombo 匹配、ShortcutRegistry 注册/解绑/作用域、Application 集成。
#include <cstdio>

#include "aurora/app/application.h"
#include "aurora/app/scene.h"
#include "aurora/app/shortcuts.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/widget/text.h"
#include "test_harness.h"

using aurora::Application;
using aurora::KeyAction;
using aurora::KeyCode;
using aurora::KeyCombo;
using aurora::KeyEvent;
using aurora::ModifierKey;
using aurora::Node;
using aurora::Scene;
using aurora::ShortcutRegistry;
using aurora::ShortcutScope;
using aurora::Text;

namespace {

auto make_key_event(KeyCode key, ModifierKey mods = ModifierKey::None, KeyAction action = KeyAction::Down) -> KeyEvent {
    KeyEvent e;
    e.key_ = static_cast<int>(key);
    e.modifiers_ = mods;
    e.action_ = action;
    return e;
}

}  // namespace

AURORA_TEST() {
    // ---- 1. KeyCombo 匹配 ----
    {
        KeyCombo combo{ModifierKey::Control, KeyCode::O};

        auto e1 = make_key_event(KeyCode::O, ModifierKey::Control);
        AURORA_TEST_CHECK(combo.matches(e1));

        // 无修饰键不匹配
        auto e2 = make_key_event(KeyCode::O);
        AURORA_TEST_CHECK(!combo.matches(e2));

        // 键码不匹配
        auto e3 = make_key_event(KeyCode::P, ModifierKey::Control);
        AURORA_TEST_CHECK(!combo.matches(e3));

        // Up 事件不匹配
        auto e4 = make_key_event(KeyCode::O, ModifierKey::Control, KeyAction::Up);
        AURORA_TEST_CHECK(!combo.matches(e4));

        // 多余修饰键不匹配（Ctrl+Shift+O != Ctrl+O）
        auto e5 = make_key_event(KeyCode::O, ModifierKey::Control | ModifierKey::Shift);
        AURORA_TEST_CHECK(!combo.matches(e5));
    }

    // ---- 2. KeyCombo 无修饰键（单键快捷键，如 F1）----
    {
        KeyCombo combo{KeyCode::F1};
        auto e = make_key_event(KeyCode::F1);
        AURORA_TEST_CHECK(combo.matches(e));
    }

    // ---- 3. KeyCombo to_string ----
    {
        KeyCombo c1{ModifierKey::Control, KeyCode::O};
        AURORA_TEST_CHECK(c1.to_string() == "Ctrl+O");

        KeyCombo c2{ModifierKey::Control | ModifierKey::Shift, KeyCode::S};
        AURORA_TEST_CHECK(c2.to_string() == "Ctrl+Shift+S");

        KeyCombo c3{KeyCode::F5};
        AURORA_TEST_CHECK(c3.to_string() == "F5");
    }

    // ---- 4. ShortcutRegistry 注册与触发 ----
    {
        ShortcutRegistry reg;
        int fired = 0;
        reg.add(KeyCombo{ModifierKey::Control, KeyCode::S}, [&fired]() -> void { ++fired; });
        AURORA_TEST_CHECK(reg.count() == 1);

        auto e = make_key_event(KeyCode::S, ModifierKey::Control);
        AURORA_TEST_CHECK(reg.handle(e));
        AURORA_TEST_CHECK(fired == 1);

        // 不匹配的键不触发
        auto e2 = make_key_event(KeyCode::A);
        AURORA_TEST_CHECK(!reg.handle(e2));
        AURORA_TEST_CHECK(fired == 1);
    }

    // ---- 5. 解绑 ----
    {
        ShortcutRegistry reg;
        int fired = 0;
        const int id = reg.add(KeyCombo{KeyCode::F2}, [&fired]() -> void { ++fired; });
        reg.remove(id);
        AURORA_TEST_CHECK(reg.count() == 0);

        auto e = make_key_event(KeyCode::F2);
        AURORA_TEST_CHECK(!reg.handle(e));
        AURORA_TEST_CHECK(fired == 0);
    }

    // ---- 6. 启用/禁用 ----
    {
        ShortcutRegistry reg;
        int fired = 0;
        const int id = reg.add(KeyCombo{KeyCode::F3}, [&fired]() -> void { ++fired; });

        reg.set_enabled(id, false);
        auto e = make_key_event(KeyCode::F3);
        AURORA_TEST_CHECK(!reg.handle(e));
        AURORA_TEST_CHECK(fired == 0);

        reg.set_enabled(id, true);
        AURORA_TEST_CHECK(reg.handle(e));
        AURORA_TEST_CHECK(fired == 1);
    }

    // ---- 7. Focus 作用域 ----
    {
        ShortcutRegistry reg;
        int fired = 0;
        reg.add(KeyCombo{ModifierKey::Control, KeyCode::C}, [&fired]() -> void { ++fired; }, ShortcutScope::Focus);

        auto e = make_key_event(KeyCode::C, ModifierKey::Control);
        // 无焦点时不触发
        AURORA_TEST_CHECK(!reg.handle(e, false));
        AURORA_TEST_CHECK(fired == 0);
        // 有焦点时触发
        AURORA_TEST_CHECK(reg.handle(e, true));
        AURORA_TEST_CHECK(fired == 1);
    }

    // ---- 8. 多绑定顺序匹配（先注册先匹配）----
    {
        ShortcutRegistry reg;
        int first = 0;
        int second = 0;
        reg.add(KeyCombo{KeyCode::F4}, [&first]() -> void { ++first; });
        reg.add(KeyCombo{KeyCode::F4}, [&second]() -> void { ++second; });

        auto e = make_key_event(KeyCode::F4);
        (void)reg.handle(e);
        AURORA_TEST_CHECK(first == 1);
        AURORA_TEST_CHECK(second == 0);  // 第一个匹配即消费
    }

    // ---- 9. bindings() 枚举与 clear ----
    {
        ShortcutRegistry reg;
        reg.add(KeyCombo{KeyCode::F5}, []() -> void {}, ShortcutScope::Global, "refresh");
        reg.add(KeyCombo{KeyCode::F6}, []() -> void {});

        auto list = reg.bindings();
        AURORA_TEST_CHECK(list.size() == 2);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(list[0].description == "refresh");

        reg.clear();
        AURORA_TEST_CHECK(reg.count() == 0);
    }

    // ---- 10. Application 集成：dispatch_key 快捷键优先 ----
    {
        auto text = Text();
        text.content = aurora::LocalizedString{"hi"};
        Scene scene{Node(std::move(text))};
        Application app{std::move(scene), 320, 240};

        int fired = 0;
        app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::K}, [&fired]() -> void { ++fired; });

        auto e = make_key_event(KeyCode::K, ModifierKey::Control);
        const bool consumed = app.dispatch_key(e);
        AURORA_TEST_CHECK(consumed);
        AURORA_TEST_CHECK(fired == 1);

        // 不匹配的键走正常派发路径
        auto e2 = make_key_event(KeyCode::J);
        app.dispatch_key(e2);
        AURORA_TEST_CHECK(fired == 1);
    }
}