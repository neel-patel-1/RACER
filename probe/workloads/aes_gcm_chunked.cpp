#include "aes_gcm_chunked.h"

#include <algorithm>

IppStatus racer_aes_gcm_decrypt_chunked(const Ipp8u *ciphertext, Ipp8u *plaintext, int length,
                                        int chunk_bytes, IppsAES_GCMState *state) {
  for (int offset = 0; offset < length; offset += chunk_bytes) {
    int bytes_in_chunk = std::min(chunk_bytes, length - offset);
    IppStatus status =
        ippsAES_GCMDecrypt(ciphertext + offset, plaintext + offset, bytes_in_chunk, state);
    if (status != ippStsNoErr) {
      return status;
    }
  }
  return ippStsNoErr;
}
