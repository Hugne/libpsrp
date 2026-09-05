/** @file
 * psrp_crypto.h - session key exchange and SecureString protection
 * ([MS-PSRP] 2.2.2.3-5, 2.2.5.1.24).
 *
 * PSRP protects SecureString values with a session key. The exchange is:
 *
 *   server -> PUBLIC_KEY_REQUEST      (an empty string, 2.2.2.5)
 *   client -> PUBLIC_KEY              (our RSA public key, 2.2.2.3)
 *   server -> ENCRYPTED_SESSION_KEY   (an AES-256 key, RSA-encrypted, 2.2.2.4)
 *
 * After that the client can decrypt SecureStrings the server sends and
 * encrypt ones it sends back. A SecureString cannot be exchanged before the
 * key is established; the spec is explicit that the exchange MUST happen
 * first.
 *
 * Both blob layouts are the CryptoAPI ones and are little-endian throughout,
 * which is the opposite of how CNG hands the key material over. The
 * conversion is done here rather than left to callers.
 */
#ifndef PSRP_CRYPTO_H
#define PSRP_CRYPTO_H

#include "psrp/psrp_buffer.h"
#include "psrp/psrp_object.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sizes fixed by the spec: a 2048-bit RSA key and a 256-bit AES key. */
#define PSRP_RSA_MODULUS_BYTES 256
#define PSRP_PUBLIC_KEY_BLOB_BYTES (16 + 4 + PSRP_RSA_MODULUS_BYTES)  /* 276 */
#define PSRP_SESSION_KEY_BYTES 32
#define PSRP_AES_BLOCK_BYTES 16

typedef struct psrp_crypto psrp_crypto_t;

/** Creates a context and generates a fresh 2048-bit RSA key pair. */
psrp_result_t psrp_crypto_new(psrp_crypto_t **out);
void psrp_crypto_free(psrp_crypto_t *c);

/** Exports the public key in the 276-byte CryptoAPI PUBLICKEYBLOB layout the
 * PUBLIC_KEY message carries: a 16-byte header, then a little-endian public
 * exponent and modulus. */
psrp_result_t psrp_crypto_export_public_key(psrp_crypto_t *c,
                                            psrp_buffer_t *out);

/** Decrypts the session key from an ENCRYPTED_SESSION_KEY blob (a CryptoAPI
 * SIMPLEBLOB: a 12-byte header then the RSA-encrypted key, little-endian).
 * On success the context can encrypt and decrypt SecureStrings. */
psrp_result_t psrp_crypto_import_session_key(psrp_crypto_t *c,
                                             const void *blob, size_t len);

/** True once a session key is available. */
bool psrp_crypto_has_session_key(const psrp_crypto_t *c);

/** Installs a known session key directly. Exists so tests (and any caller that
 * obtained the key another way) need not perform an RSA exchange. */
psrp_result_t psrp_crypto_set_session_key(psrp_crypto_t *c, const void *key,
                                          size_t len);

/** SecureString protection: AES-256-CBC under the session key. The plaintext
 * is the string's UTF-16LE bytes, matching what PowerShell encrypts. */
psrp_result_t psrp_crypto_encrypt(psrp_crypto_t *c, const void *plaintext,
                                  size_t len, psrp_buffer_t *out);
psrp_result_t psrp_crypto_decrypt(psrp_crypto_t *c, const void *ciphertext,
                                  size_t len, psrp_buffer_t *out);

/** Convenience wrappers that convert to and from UTF-8 for the caller. Both
 * work in raw ciphertext bytes; see the value-level pair below for what goes
 * on the wire. */
psrp_result_t psrp_crypto_encrypt_string(psrp_crypto_t *c, const char *utf8,
                                         size_t len, psrp_buffer_t *out);
psrp_result_t psrp_crypto_decrypt_string(psrp_crypto_t *c,
                                         const void *ciphertext, size_t len,
                                         psrp_buffer_t *out);

/** The wire form. A Secure String element (2.2.5.1.24) carries the ciphertext
 * as base64, so a value built from raw encrypt output is not valid XML and the
 * message fails to serialize. These produce and consume the value directly,
 * which is the only shape a caller should ever need.
 *
 * psrp_crypto_protect_string makes a PSRP_VAL_SECURESTRING ready to be used as
 * a command parameter, pipeline input, or credential password.
 * psrp_crypto_unprotect_value reads one back, for example from a
 * PSRP_EVENT_PIPELINE_OUTPUT carrying a SecureString the server returned.
 * Both require the session key to be in place. */
psrp_result_t psrp_crypto_protect_string(psrp_crypto_t *c, const char *utf8,
                                         size_t len, psrp_value_t *out);
psrp_result_t psrp_crypto_unprotect_value(psrp_crypto_t *c,
                                          const psrp_value_t *secure_string,
                                          psrp_buffer_t *out_utf8);

/* ------------------------------------------- key exchange messages ------ */

/** 2.2.2.3: a complex object with a base64 PublicKey property. */
psrp_result_t psrp_build_public_key(psrp_crypto_t *c, psrp_buffer_t *out);

/** 2.2.2.4: reads EncryptedSessionKey and installs the key in `c`. */
psrp_result_t psrp_parse_encrypted_session_key(psrp_crypto_t *c,
                                               const void *xml, size_t n);

/** 2.2.2.5: the payload is a serialized empty string. */
psrp_result_t psrp_build_public_key_request(psrp_buffer_t *out);
bool psrp_is_public_key_request(const void *xml, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_CRYPTO_H */
