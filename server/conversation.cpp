#include "conversation.h"

#include <algorithm>

namespace bmoe::server {

TurnPlan plan_turn(const std::vector<ChatMessage> & history, const std::vector<ChatMessage> & m) {
    TurnPlan t;
    const bool extends = !history.empty() && m.size() == history.size() + 1 && m.back().role == "user" &&
                         std::equal(history.begin(), history.end(), m.begin());
    if (extends) {
        t.prompt = m.back().content;
        t.clear_kv = false;
        return t;
    }

    size_t last_user = m.size();
    for (size_t i = m.size(); i-- > 0;)
        if (m[i].role == "user") {
            last_user = i;
            break;
        }
    if (last_user == m.size()) {
        t.ok = false;
        t.error = "the conversation has no user message";
        return t;
    }
    t.prompt = m[last_user].content;
    // One user turn per generation, so a leading system message rides along with it. Anything else
    // before the last user message cannot be replayed into a fresh KV.
    size_t replayable = 0;
    if (m[0].role == "system" && last_user > 0) {
        t.prompt = m[0].content + "\n\n" + t.prompt;
        replayable = 1;
    }
    t.history_dropped = last_user > replayable;
    return t;
}

} // namespace bmoe::server
