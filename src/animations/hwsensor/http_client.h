#ifndef HWSENSOR_HTTP_CLIENT_H
#define HWSENSOR_HTTP_CLIENT_H

#include <stddef.h>

typedef struct {
    char* body;       /* malloc'd, NUL-terminated */
    size_t body_len;
    long status_code;
} http_response;

/* Loopback-only HTTP/1.0 GET over a plain POSIX socket -- no TLS, no
 * redirects, no chunked-transfer decoding (HTTP/1.0 + "Connection: close"
 * sidesteps that entirely: the response is just everything read until the
 * peer closes the connection). Returns 0 and fills *out on success; on any
 * failure (connect refused/timeout, read timeout/error, malformed
 * response) returns a negative code and *out is zeroed -- callers must
 * treat any nonzero return as "no data available", never fatal. */
int http_get_loopback(unsigned short port, const char* path, int connect_timeout_ms, int recv_timeout_ms, http_response* out);
void http_response_free(http_response* out);

#endif /* HWSENSOR_HTTP_CLIENT_H */
