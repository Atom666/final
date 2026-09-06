#include <mirror/tls.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <openssl/err.h>

void tls_global_init(void) {
    OPENSSL_init_ssl(0, NULL);
}

static void ssl_err(char *err, size_t err_len, const char *prefix) {
    unsigned long e = ERR_get_error();
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    snprintf(err, err_len, "%s: %s", prefix, e ? buf : strerror(errno));
}

SSL_CTX *tls_create_client_ctx(const struct tls_client_config *cfg, char *err, size_t err_len) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { ssl_err(err, err_len, "SSL_CTX_new"); return NULL; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (!cfg->insecure && cfg->ca_file[0] && SSL_CTX_load_verify_locations(ctx, cfg->ca_file, NULL) != 1) {
        ssl_err(err, err_len, "load CA"); SSL_CTX_free(ctx); return NULL;
    }
    if (!cfg->insecure && cfg->verify_peer && !cfg->ca_file[0] && SSL_CTX_set_default_verify_paths(ctx) != 1) {
        ssl_err(err, err_len, "load default CA paths"); SSL_CTX_free(ctx); return NULL;
    }
    if (cfg->cert_file[0] && SSL_CTX_use_certificate_file(ctx, cfg->cert_file, SSL_FILETYPE_PEM) != 1) {
        ssl_err(err, err_len, "load client cert"); SSL_CTX_free(ctx); return NULL;
    }
    if (cfg->key_file[0] && SSL_CTX_use_PrivateKey_file(ctx, cfg->key_file, SSL_FILETYPE_PEM) != 1) {
        ssl_err(err, err_len, "load client key"); SSL_CTX_free(ctx); return NULL;
    }
    if (cfg->cert_file[0] && SSL_CTX_check_private_key(ctx) != 1) {
        ssl_err(err, err_len, "client certificate/key mismatch"); SSL_CTX_free(ctx); return NULL;
    }
    SSL_CTX_set_verify(ctx, (cfg->verify_peer && !cfg->insecure) ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    return ctx;
}

SSL_CTX *tls_create_server_ctx(const struct tls_server_config *cfg, char *err, size_t err_len) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { ssl_err(err, err_len, "SSL_CTX_new"); return NULL; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, cfg->cert_file, SSL_FILETYPE_PEM) != 1) {
        ssl_err(err, err_len, "load server cert"); SSL_CTX_free(ctx); return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, cfg->key_file, SSL_FILETYPE_PEM) != 1) {
        ssl_err(err, err_len, "load server key"); SSL_CTX_free(ctx); return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        ssl_err(err, err_len, "server certificate/key mismatch"); SSL_CTX_free(ctx); return NULL;
    }
    if (!cfg->insecure && cfg->client_ca_file[0] && SSL_CTX_load_verify_locations(ctx, cfg->client_ca_file, NULL) != 1) {
        ssl_err(err, err_len, "load client CA"); SSL_CTX_free(ctx); return NULL;
    }
    int mode = SSL_VERIFY_NONE;
    if (cfg->require_client_cert && !cfg->insecure) mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    SSL_CTX_set_verify(ctx, mode, NULL);
    return ctx;
}

int tls_write_all_counted(SSL *ssl, const void *buf, size_t len,
                          uint64_t *ssl_write_calls) {
    const uint8_t *p = buf;
    while (len > 0) {
        if (ssl_write_calls) (*ssl_write_calls)++;
        int n = SSL_write(ssl, p, len > INT32_MAX ? INT32_MAX : (int)len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int tls_write_all(SSL *ssl, const void *buf, size_t len) {
    return tls_write_all_counted(ssl, buf, len, NULL);
}

int tls_read_exact(SSL *ssl, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        int n = SSL_read(ssl, p, len > INT32_MAX ? INT32_MAX : (int)len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}
