#include "fl_random.h"
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
bool fl_random_bytes(void *out, size_t size)
{
    return size <= 0xffffffffu && BCryptGenRandom(NULL, out, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
#elif defined(__APPLE__)
#include <stdlib.h>
bool fl_random_bytes(void *out, size_t size)
{
    arc4random_buf(out, size);
    return true;
}
#else
bool fl_random_bytes(void *out, size_t size)
{
    FILE *file = fopen("/dev/urandom", "rb");
    if (!file) return false;
    bool ok = fread(out, 1, size, file) == size;
    fclose(file);
    return ok;
}
#endif
