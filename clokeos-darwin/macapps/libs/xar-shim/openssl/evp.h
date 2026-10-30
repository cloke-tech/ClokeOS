/* Minimaler Ersatz für <openssl/evp.h>: nur die Digest-Funktionen, die libxar braucht
 * (md5, sha1, sha256, sha512). ClokeOS hat kein OpenSSL im Abbild. */
#ifndef CLOKE_EVP_SHIM_H
#define CLOKE_EVP_SHIM_H
#include <stddef.h>
#include <stdint.h>

#define EVP_MAX_MD_SIZE 64

typedef struct evp_md_st EVP_MD;
typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct engine_st ENGINE;

void OpenSSL_add_all_digests(void);
const EVP_MD *EVP_get_digestbyname(const char *name);
EVP_MD_CTX *EVP_MD_CTX_create(void);
void EVP_MD_CTX_destroy(EVP_MD_CTX *ctx);
const EVP_MD *EVP_MD_CTX_md(const EVP_MD_CTX *ctx);
int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *md, ENGINE *impl);
int EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt);
int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s);
int EVP_MD_size(const EVP_MD *md);
int EVP_MD_nid(const EVP_MD *md);
const char *OBJ_nid2ln(int nid);
#endif
