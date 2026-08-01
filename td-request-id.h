#ifndef TD_REQUEST_ID_H
#define TD_REQUEST_ID_H

#include <cstdint>

namespace td_request_id {

constexpr std::uint64_t PUBLIC_MAX =
    (std::uint64_t{1} << 63) - 1;
constexpr std::uint64_t ACTIVATE =
    std::uint64_t{1} << 63;
constexpr std::uint64_t CLOSE =
    (std::uint64_t{1} << 63) | std::uint64_t{1};

inline bool isPublic(std::uint64_t requestId)
{
    return requestId != 0 && requestId <= PUBLIC_MAX;
}

inline bool isControl(std::uint64_t requestId)
{
    return requestId == ACTIVATE || requestId == CLOSE;
}

inline std::uint64_t nextPublic(std::uint64_t requestId)
{
    return requestId >= PUBLIC_MAX ? 1 : requestId + 1;
}

} // namespace td_request_id

#endif
