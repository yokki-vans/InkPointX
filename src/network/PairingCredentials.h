#pragma once

#include <cstddef>

namespace PairingCredentials {

// The short code is typed by a person and is valid only for the lifetime of
// the file-transfer server. A successful pairing is exchanged for the longer
// random session token stored in an HttpOnly cookie.
inline constexpr size_t CODE_LENGTH = 8;
inline constexpr size_t SESSION_TOKEN_LENGTH = 32;

}  // namespace PairingCredentials
