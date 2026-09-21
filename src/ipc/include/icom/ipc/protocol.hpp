#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace icom::ipc
{

// Deliberately a flat text protocol, not anything binary/versioned: the
// only client is `intercomctl` (or a human with `socat`/`nc -U`), so
// grep-ability from a shell beats a schema. One command per line in, one
// response line out:
//
//   request:   "<COMMAND> [ARG ...]\n"
//   response:  "OK [message]\n"   or   "ERR <message>\n"
    struct CommandResult
    {
        bool ok = false;
        std::string message;

        static CommandResult success(std::string message = {}) { return {true, std::move(message)}; }
        static CommandResult failure(std::string message) { return {false, std::move(message)}; }
    };

    using CommandHandler = std::function<CommandResult(const std::vector<std::string>& args)>;

// Splits on ASCII whitespace; no quoting support, matching the "simple
// enough to type by hand" goal above.
    std::vector<std::string> tokenize(std::string_view line);

    std::string formatResponse(const CommandResult& result);

} // namespace icom::ipc
