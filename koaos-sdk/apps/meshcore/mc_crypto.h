/* mc_crypto.h -- the crypto MeshCore needs that the KoaOS ABI does not provide:
 * SHA-512, Ed25519 (MeshCore's expanded 64-byte private key format) and the
 * Ed25519 -> X25519 ECDH that MeshCore uses for direct messages.
 * Compact, portable C (TweetNaCl-style field arithmetic). Slow but small. */
#ifndef MC_CRYPTO_H
#define MC_CRYPTO_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint64_t h[8]; uint8_t buf[128]; uint32_t n; uint64_t total; } mc_sha512_t;
void mc_sha512_init(mc_sha512_t *c);
void mc_sha512_update(mc_sha512_t *c, const void *d, size_t n);
void mc_sha512_final(mc_sha512_t *c, uint8_t out[64]);

/* seed(32) -> prv(64, expanded+clamped, same layout as MeshCore/orlp) + pub(32) */
void mc_ed_keypair(const uint8_t seed[32], uint8_t prv[64], uint8_t pub[32]);
/* sign with an expanded private key */
void mc_ed_sign(uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pub[32], const uint8_t prv[64]);
/* 0 = valid */
int  mc_ed_verify(const uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pub[32]);
/* shared secret = X25519(prv scalar, montgomery(pub)); same as orlp ed25519_key_exchange */
int  mc_ed_ecdh(uint8_t shared[32], const uint8_t their_pub[32], const uint8_t my_prv[64]);
#endif
