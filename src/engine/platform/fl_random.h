#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Fill out with cryptographically secure random bytes from the operating system. False on failure. */
bool fl_random_bytes(void *out, size_t size);
