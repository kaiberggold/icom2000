#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace icom::core
{

    struct JournalField
    {
        std::string_view name;
        std::string_view value;
    };

    // Encodes one entry in systemd-journald's native protocol
    // (https://systemd.io/JOURNAL_NATIVE_PROTOCOL/) -- what libsystemd's
    // sd_journal_send() puts on the wire, done here instead so the project
    // doesn't need libsystemd at build time (see docs/CROSS_COMPILE.md).
    // Field names must be uppercase letters, digits and underscores, not
    // starting with an underscore; values may contain anything, newlines
    // included.
    std::string encodeJournalEntry(std::initializer_list<JournalField> fields);

    inline constexpr const char* JOURNAL_SOCKET_PATH = "/run/systemd/journal/socket";

    // Sends an encoded entry to journald as one datagram. Never blocks: if
    // the socket doesn't exist or journald's queue is full, the entry is
    // dropped and this returns false.
    bool sendJournalEntry(std::string_view entry, const std::string& socketPath = JOURNAL_SOCKET_PATH);

} // namespace icom::core
