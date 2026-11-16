#ifndef AES_GCM_CHUNKED_H
#define AES_GCM_CHUNKED_H

#include "ippcp.h"

// Decrypts length bytes of AES-GCM ciphertext with one ippsAES_GCMDecrypt call
// for each chunk_bytes. The output is the same as one call over all bytes.
//
// On CPUs with VAES, ipp-crypto decrypts in assembly that has no probe points,
// so one call over a large buffer cannot yield. This file is compiled through
// the plugin, so the loop gets a probe point between the chunks.
IppStatus racer_aes_gcm_decrypt_chunked(const Ipp8u *ciphertext, Ipp8u *plaintext, int length,
                                        int chunk_bytes, IppsAES_GCMState *state);

#endif
