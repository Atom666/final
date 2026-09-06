#ifndef MIRROR_TLS_H
#define MIRROR_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <openssl/ssl.h>

struct tls_client_config {
    const char *ca_file;
    const char *cert_file;
    const char *key_file;
    const char *server_name;
    bool verify_peer;
    bool insecure;
};

struct tls_server_config {
    const char *cert_file;
    const char *key_file;
    const char *client_ca_file;
    bool require_client_cert;
    bool insecure;
};

void tls_global_init(void);
SSL_CTX *tls_create_client_ctx(const struct tls_client_config *cfg, char *err, size_t err_len);
SSL_CTX *tls_create_server_ctx(const struct tls_server_config *cfg, char *err, size_t err_len);
int tls_write_all(SSL *ssl, const void *buf, size_t len);
int tls_write_all_counted(SSL *ssl, const void *buf, size_t len,
                          uint64_t *ssl_write_calls);
int tls_read_exact(SSL *ssl, void *buf, size_t len);

#endif
