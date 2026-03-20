#include <stddef.h>
#include <stdbool.h>
#define NRF_STATIC_INLINE __STATIC_INLINE
#define NRFX_ASSERT(expression) if (!(expression)) while (1) {}

