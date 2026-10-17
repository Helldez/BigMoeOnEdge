#include "chat_parse.h"
#include "bmoe/log.h"

#include <cstdio>
#include <mutex>

namespace bmoe::detail {

common_chat_parser_params build_parse_params(const common_chat_params & cp) {
    common_chat_parser_params pp(cp);
    // The converting constructor drops the grammar; load it so the parser can actually match.
    pp.parser.load(cp.parser);
    pp.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
    return pp;
}

void warn_parse_failed_once(const char * what) {
    static std::once_flag flag;
    std::call_once(flag, [what] {
        log_printf(LogLevel::Warn, "reasoning parse failed (%s); showing raw model output", what ? what : "unknown");
    });
}

} // namespace bmoe::detail
