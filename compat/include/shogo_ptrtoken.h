// 64-bit support: the 1998 game code squeezes pointers into 32-bit DDWORD
// message fields (dialog queues, the in-game dialog's object handle).
// On a 64-bit build those pointers are swapped for small tokens from a
// per-module table, so the 32-bit message layout stays unchanged.
#ifndef SHOGO_PTRTOKEN_H
#define SHOGO_PTRTOKEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Returns a nonzero token for p (0 for NULL). The same pointer always maps to the same token.
uint32_t shogo_ptr_to_token(const void *p);
// Returns the pointer registered for token, or NULL if unknown.
void *shogo_token_to_ptr(uint32_t token);
#ifdef __cplusplus
}
#endif

#define SHOGO_PTR_TO_TOKEN(p) shogo_ptr_to_token((const void *)(p))
#define SHOGO_TOKEN_TO_PTR(t) shogo_token_to_ptr((uint32_t)(t))

#endif
