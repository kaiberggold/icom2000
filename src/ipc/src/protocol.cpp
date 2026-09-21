#include "icom/ipc/protocol.hpp"

#include <cctype>
#include <sstream>

namespace icom::ipc
{

    std::vector<std::string> tokenize(std::string_view line)
    {
        std::vector<std::string> tokens;
        std::size_t i = 0;
        while (i < line.size())
        {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            {
                ++i;
            }
            std::size_t start = i;
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
            {
                ++i;
            }
            if (i > start)
            {
                tokens.emplace_back(line.substr(start, i - start));
            }
        }
        return tokens;
    }

    std::string formatResponse(const CommandResult& result)
    {
        return (result.ok ? "OK " : "ERR ") + result.message + "\n";
    }

} // namespace icom::ipc
