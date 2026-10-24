#include "OnlineKey.h"
#include "B64.h"
#include <string.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/bignum.h>

#if defined(ESP_PLATFORM)
#include <esp_random.h>
void OnlineKey::fill(uint8_t* out, size_t n) { esp_fill_random(out, n); }
#else
#include <stdio.h>
void OnlineKey::fill(uint8_t* out, size_t n) {
  FILE* f = fopen("/dev/urandom", "rb");
  if (!f || fread(out, 1, n, f) != n) memset(out, 0x5a, n);
  if (f) fclose(f);
}
#endif

static int rng(void*, unsigned char* out, size_t n) { OnlineKey::fill(out, n); return 0; }

bool OnlineKey::generate(uint8_t d[32]) {
  mbedtls_ecp_group g; mbedtls_mpi k; mbedtls_ecp_point q;
  mbedtls_ecp_group_init(&g); mbedtls_mpi_init(&k); mbedtls_ecp_point_init(&q);
  bool ok = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_gen_keypair(&g, &k, &q, rng, nullptr) == 0 &&
            mbedtls_mpi_write_binary(&k, d, 32) == 0;
  mbedtls_ecp_group_free(&g); mbedtls_mpi_free(&k); mbedtls_ecp_point_free(&q);
  return ok;
}

bool OnlineKey::publicKey(const uint8_t d[32], uint8_t pub[65]) {
  mbedtls_ecp_group g; mbedtls_mpi k; mbedtls_ecp_point q;
  mbedtls_ecp_group_init(&g); mbedtls_mpi_init(&k); mbedtls_ecp_point_init(&q);
  size_t olen = 0;
  bool ok = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_mpi_read_binary(&k, d, 32) == 0 &&
            mbedtls_ecp_check_privkey(&g, &k) == 0 &&
            mbedtls_ecp_mul(&g, &q, &k, &g.G, rng, nullptr) == 0 &&
            mbedtls_ecp_point_write_binary(&g, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub, 65) == 0 &&
            olen == 65;
  mbedtls_ecp_group_free(&g); mbedtls_mpi_free(&k); mbedtls_ecp_point_free(&q);
  return ok;
}

bool OnlineKey::sign(const uint8_t d[32], const char* msg, uint8_t sig[64]) {
  uint8_t h[32];
  mbedtls_sha256((const unsigned char*)msg, strlen(msg), h, 0);
  mbedtls_ecp_group g; mbedtls_mpi k, r, s;
  mbedtls_ecp_group_init(&g); mbedtls_mpi_init(&k); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
  bool ok = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_mpi_read_binary(&k, d, 32) == 0 &&
            mbedtls_ecdsa_sign(&g, &r, &s, &k, h, 32, rng, nullptr) == 0 &&
            mbedtls_mpi_write_binary(&r, sig, 32) == 0 &&
            mbedtls_mpi_write_binary(&s, sig + 32, 32) == 0;
  mbedtls_ecp_group_free(&g); mbedtls_mpi_free(&k); mbedtls_mpi_free(&r); mbedtls_mpi_free(&s);
  return ok;
}

void OnlineKey::sha256hex(const char* msg, char out[65]) {
  uint8_t h[32];
  mbedtls_sha256((const unsigned char*)msg, strlen(msg), h, 0);
  hexencode(h, 32, out);
}
