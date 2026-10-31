# Mbed TLS 3.6.7 (LTS)

- Source: https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7
  (`mbedtls-3.6.7.tar.bz2`, SHA-256
  `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`,
  as the release's `mbedtls-3.6.7-sha256sum.txt` gives it).
- Kept: `LICENSE`, `include/`, `library/` (`.c` and `.h` only), unchanged.
- License: Apache-2.0 OR GPL-2.0-or-later; Alembic uses it under
  Apache-2.0, which GPL-3.0 accepts.
- `alembic_config.h` (ours) is the build's `MBEDTLS_USER_CONFIG_FILE`.
- Updating: replace `include/` and `library/` from a newer 3.6.x release,
  update this file, rebuild, run the TLS tests (`tests/test_tls.cpp`).
