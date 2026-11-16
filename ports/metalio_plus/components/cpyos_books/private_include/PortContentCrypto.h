#pragma once

// Port-supplied content-protection crypto backend.
//
// The shared glue in lib/Epub/ContentProtection.cpp selects this header when
// FREEINK_CONTENT_PORT_PSA is defined. Only the read path is implemented:
// AES-128-CBC decrypt plus SHA-1/SHA-256 and random bytes. The legacy
// client-side RSA/PKCS#12 helpers have no PSA implementation here and refuse.

#include <Crypto.h>

namespace freeink {
namespace content {

Crypto& portContentCrypto();

// Round-trips one AES-128-CBC block and checks SHA-256("abc"). Called once at
// boot so a hardware session can tell "PSA works" from "protected books fail
// for some other reason". Returns true on success; details are logged.
bool portContentCryptoSelfTest();

}  // namespace content
}  // namespace freeink
