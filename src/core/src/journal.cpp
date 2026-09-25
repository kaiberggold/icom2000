#include "icom/core/journal.hpp"

#include <cstdint>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>

namespace icom::core
{

    std::string encodeJournalEntry(std::initializer_list<JournalField> fields)
    {
        std::string entry;
        for (const JournalField& field : fields)
        {
            entry.append(field.name);
            if (field.value.find('\n') == std::string_view::npos)
            {
                entry.push_back('=');
            }
            else
            {
                // A value containing a newline can't use NAME=value -- the
                // protocol's binary form is NAME, newline, the value's length
                // as a 64-bit little-endian integer, then the raw value.
                entry.push_back('\n');
                std::uint64_t size = field.value.size();
                for (int i = 0; i < 8; ++i)
                {
                    entry.push_back(static_cast<char>(size & 0xff));
                    size >>= 8;
                }
            }
            entry.append(field.value);
            entry.push_back('\n');
        }
        return entry;
    }

    bool sendJournalEntry(std::string_view entry, const std::string& socketPath)
    {
        static const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
        {
            return false;
        }

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (socketPath.size() >= sizeof(address.sun_path))
        {
            return false;
        }
        std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);

        const ssize_t sent = ::sendto(fd, entry.data(), entry.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
                                      reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        return sent == static_cast<ssize_t>(entry.size());
    }

} // namespace icom::core
