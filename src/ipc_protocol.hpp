#ifndef SEASIDE_IPC_PROTOCOL_HPP
#define SEASIDE_IPC_PROTOCOL_HPP

#include "config_edit.hpp"
#include "ipc.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <sys/un.h>

namespace seaside::ipc {

inline constexpr std::size_t request_limit = 64 * 1024;
inline constexpr std::size_t record_limit = 1024 * 1024;
inline constexpr std::uint64_t id_limit = 9007199254740991;

struct ParsedRecord {
    IpcJson object;
    std::optional<std::uint64_t> id;
    std::string_view error;
};

ParsedRecord parse_record(std::string_view record);
std::optional<IpcError> validate_request(const ParsedRecord &record);
std::expected<SettingEdit, IpcError> decode_setting_edit(const IpcJson &params);
// An empty result means serialization would exceed the record limit.
std::optional<std::string> serialize(const IpcJson &object);

std::expected<std::string, std::string> runtime_path(std::string_view display);
std::expected<sockaddr_un, std::string> address(std::string_view path);

} // namespace seaside::ipc
#endif
