#pragma once

#include "aes.h"

// Match the unsupported protected-book crypto stubs in aes.h.
inline int wc_Sha256Hash(const byte *, word32, byte *) { return -1; }
