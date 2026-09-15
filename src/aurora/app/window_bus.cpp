#include "aurora/app/window_bus.h"

#include <algorithm>

namespace aurora {

auto WindowEventBus::erase_token(State &st, Token token) -> void {
    for (auto it = st.subs.begin(); it != st.subs.end();) {
        auto &entries = it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [token](const Entry &e) -> bool { return e.token == token; }),
                      entries.end());
        // 清空后的类型槽一并移除，避免 subscriber_count 之外的容器无界增长。
        if (entries.empty()) {
            it = st.subs.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace aurora
