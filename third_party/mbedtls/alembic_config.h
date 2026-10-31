/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Alembic's changes to mbedTLS's default configuration (MBEDTLS_USER_CONFIG_FILE,
 * read after include/mbedtls/mbedtls_config.h): a TLS 1.2 / 1.3 client for
 * FTPS and HTTPS WebDAV (src/engine/tls.cpp), on every platform the engine
 * runs on (PC, Windows, the PS5 payload). */

/* Sockets, files and random bytes come from the engine (net.hpp, io.hpp,
 * platform::secure_random): no POSIX calls of mbedTLS's own. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_ENTROPY_C
#undef MBEDTLS_CTR_DRBG_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

/* Several connections run at once (downloads, listings, other jobs). */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

/* A client only: no server side, no DTLS. */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_SRTP
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_EARLY_DATA

/* Not needed for a client that pins certificates. */
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CRL_PARSE_C
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_PK_WRITE_C
#undef MBEDTLS_DEBUG_C
#undef MBEDTLS_SELF_TEST
