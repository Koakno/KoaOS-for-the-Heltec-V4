/* Prints hex results of mc_crypto for fixed inputs; tests/mc_crypto_check.py compares them with python-cryptography. */
#include <stdio.h>
#include <string.h>
#include "../apps/meshcore/mc_crypto.h"
static void hex(const char *l, const uint8_t *p, int n) { printf("%s=", l); for (int i = 0; i < n; i++) printf("%02x", p[i]); printf("\n"); }
int main(void) {
  uint8_t seed[32], seed2[32], prv[64], pub[32], prv2[64], pub2[32], sig[64], sh1[32], sh2[32], h[64];
  for (int i = 0; i < 32; i++) { seed[i] = (uint8_t)(i * 7 + 1); seed2[i] = (uint8_t)(255 - i * 5); }
  mc_sha512_t c; mc_sha512_init(&c); mc_sha512_update(&c, "abc", 3); mc_sha512_final(&c, h); hex("sha512_abc", h, 64);
  uint8_t big[300]; for (int i = 0; i < 300; i++) big[i] = (uint8_t)i;
  mc_sha512_init(&c); mc_sha512_update(&c, big, 100); mc_sha512_update(&c, big + 100, 200); mc_sha512_final(&c, h); hex("sha512_300", h, 64);
  mc_ed_keypair(seed, prv, pub);  hex("pub1", pub, 32);  hex("prv1", prv, 64);
  mc_ed_keypair(seed2, prv2, pub2); hex("pub2", pub2, 32);
  const char *m = "hello meshcore advert";
  mc_ed_sign(sig, (const uint8_t *)m, strlen(m), pub, prv); hex("sig1", sig, 64);
  printf("verify_ok=%d\n", mc_ed_verify(sig, (const uint8_t *)m, strlen(m), pub));
  sig[5] ^= 1; printf("verify_bad=%d\n", mc_ed_verify(sig, (const uint8_t *)m, strlen(m), pub));
  mc_ed_ecdh(sh1, pub2, prv); mc_ed_ecdh(sh2, pub, prv2);
  hex("ecdh_a", sh1, 32); hex("ecdh_b", sh2, 32);
  return 0;
}
