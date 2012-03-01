/**
 * This file is part of the CernVM File System.
 *
 * This is a wrapper around OpenSSL's libcrypto.  It supports
 * signing of data with an X.509 certificate and verifiying
 * a signature against a certificate.  The certificates act only as key
 * store, there is no verification against the CA chain.
 *
 * It also supports verification of plain RSA signatures (for the whitelist).
 *
 * We work exclusively with PEM formatted files (= Base64-encoded DER files).
 */

#include "cvmfs_config.h"
#include "signature.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>

#include <string>
#include <vector>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

#include "platform.h"
#include "hash.h"
#include "util.h"
#include "smalloc.h"

using namespace std;  // NOLINT

namespace signature {

const char *kDefaultPublicKey = "/etc/cvmfs/keys/cern.ch.pub";

EVP_PKEY *private_key_ = NULL;
X509 *certificate_ = NULL;
vector<RSA *> *public_keys_;  /**< Contains cvmfs public master keys */


void Init() {
  OpenSSL_add_all_algorithms();
  public_keys_ = new vector<RSA *>();
}


void Fini() {
  EVP_cleanup();
  if (certificate_) X509_free(certificate_);
  if (private_key_) EVP_PKEY_free(private_key_);
  if (!public_keys_->empty()) {
    for (unsigned i = 0; i < public_keys_->size(); ++i)
      RSA_free((*public_keys_)[i]);
    public_keys_->clear();
  }
  delete public_keys_;
  public_keys_ = NULL;
}


/**
 * OpenSSL error strings.
 */
string GetCryptoError() {
  char buf[121];
  string err;
  while (ERR_peek_error() != 0) {
    ERR_error_string(ERR_get_error(), buf);
    err += string(buf);
  }
  return err;
}


/**
 * @param[in] file_pem File name of the PEM key file
 * @param[in] password Password for the private key.
 *     Password is not saved internally, but the private key is.
 * \return True on success, false otherwise
 */
bool LoadPrivateKeyPath(const string &file_pem, const string &password) {
  bool result;
  FILE *fp = NULL;
  char *tmp = strdupa(password.c_str());

  if ((fp = fopen(file_pem.c_str(), "r")) == NULL)
    return false;
  result = (private_key_ = PEM_read_PrivateKey(fp, NULL, NULL, tmp)) != NULL;
  fclose(fp);
  return result;
}


/**
 * Clears the memory storing the private key.
 */
void UnloadPrivateKey() {
  if (private_key_) EVP_PKEY_free(private_key_);
  private_key_ = NULL;
}


/**
 * Loads a certificate.  This certificate is used for the following
 * signature verifications
 *
 * \return True on success, false otherwise
 */
bool LoadCertificatePath(const string &file_pem) {
  if (certificate_) {
    X509_free(certificate_);
    certificate_ = NULL;
  }

  bool result;
  char *nopwd = strdupa("");
  FILE *fp;

  if ((fp = fopen(file_pem.c_str(), "r")) == NULL)
    return false;
  result = (certificate_ = PEM_read_X509_AUX(fp, NULL, NULL, nopwd)) != NULL;

  if (!result && certificate_) {
    X509_free(certificate_);
    certificate_ = NULL;
  }

  fclose(fp);
  return result;
}


/**
 * See the function that loads the certificate from file.
 */
bool LoadCertificateMem(const unsigned char *buffer,
                        const unsigned buffer_size)
{
  if (certificate_) {
    X509_free(certificate_);
    certificate_ = NULL;
  }

  bool result;
  char *nopwd = strdupa("");

  BIO *mem = BIO_new(BIO_s_mem());
  if (!mem) return false;
  if (BIO_write(mem, buffer, buffer_size) <= 0) {
    BIO_free(mem);
    return false;
  }
  result = (certificate_ = PEM_read_bio_X509_AUX(mem, NULL, NULL, nopwd))
           != NULL;
  BIO_free(mem);

  if (!result && certificate_) {
    X509_free(certificate_);
    certificate_ = NULL;
  }

  return result;
}


/**
 * Loads a list of public RSA keys separated by ":".
 */
bool LoadPublicRsaKeys(const string &file_list) {
  if (!public_keys_->empty()) {
    for (unsigned i = 0; i < public_keys_->size(); ++i)
      RSA_free((*public_keys_)[i]);
    public_keys_->clear();
  }

  if (file_list == "")
    return true;
  const vector<string> pem_files = SplitString(file_list, ':');

  char *nopwd = strdupa("");
  FILE *fp;

  for (unsigned i = 0; i < pem_files.size(); ++i) {
    if ((fp = fopen(pem_files[i].c_str(), "r")) == NULL)
      return false;
    EVP_PKEY *this_key;
    if ((this_key = PEM_read_PUBKEY(fp, NULL, NULL, nopwd)) == NULL) {
      fclose(fp);
      return false;
    }
    fclose(fp);
    public_keys_->push_back(EVP_PKEY_get1_RSA(this_key));
    EVP_PKEY_free(this_key);
    if ((*public_keys_)[i] == NULL)
      return false;
  }

  return true;
}

/**
 * Returns SHA-1 hash from DER encoded certificate, encoded the same way
 * OpenSSL does (01:AB:...).
 * Empty string on failure.
 */
string FingerprintCertificate() {
  if (!certificate_) return "";

  int buffer_size;
  unsigned char *buffer = NULL;

  buffer_size = i2d_X509(certificate_, &buffer);
  if (buffer_size < 0) return "";

  hash::Any hash(hash::kSha1);
  hash::HashMem(buffer, buffer_size, &hash);
  free(buffer);

  const string hash_str = hash.ToString();
  string result;
  for (unsigned i = 0; i < hash_str.length(); ++i) {
    if ((i > 0) && (i%2 == 0)) result += ":";
    result += toupper(hash_str[i]);
  }
  return result;
}


/**
 * \return Some human-readable information about the loaded certificate.
 */
string Whois() {
  if (!certificate_) return "No certificate loaded";

  string result;
  X509_NAME *subject = X509_get_subject_name(certificate_);
  X509_NAME *issuer = X509_get_issuer_name(certificate_);
  char *buffer = NULL;
  buffer = X509_NAME_oneline(subject, NULL, 0);
  if (buffer) {
    result = "Publisher: " + string(buffer);
    free(buffer);
  }
  buffer = X509_NAME_oneline(issuer, NULL, 0);
  if (buffer) {
    result += "\nCertificate issued by: " + string(buffer);
    free(buffer);
  }
  return result;
}


bool WriteCertificateMem(unsigned char **buffer, unsigned *buffer_size) {
  BIO *mem = BIO_new(BIO_s_mem());
  if (!mem) return false;
  if (!PEM_write_bio_X509(mem, certificate_)) {
    BIO_free(mem);
    return false;
  }

  void *bio_buffer;
  *buffer_size = BIO_get_mem_data(mem, &bio_buffer);
  *buffer = reinterpret_cast<unsigned char *>(smalloc(*buffer_size));
  memcpy(*buffer, bio_buffer, *buffer_size);
  BIO_free(mem);
  return true;
}


/**
 * Checks, whether the loaded certificate and the loaded private key match.
 *
 * \return True, if private key and certificate match, false otherwise.
 */
bool KeysMatch() {
  if (!certificate_ || !private_key_)
    return false;

  bool result = false;
  const unsigned char *sign_me = reinterpret_cast<const unsigned char *>
                                   ("sign me");
  unsigned char *signature = NULL;
  unsigned signature_size;
  if (Sign(sign_me, 7, &signature, &signature_size) &&
      Verify(sign_me, 7, signature, signature_size))
  {
    result = true;
  }
  if (signature) free(signature);
  return result;
}


/**
 * Signs a data block using the loaded private key.
 *
 * \return True on sucess, false otherwise
 */
bool Sign(const unsigned char *buffer, const unsigned buffer_size,
          unsigned char **signature, unsigned *signature_size)
{
  if (!private_key_) {
    *signature_size = 0;
    *signature = NULL;
    return false;
  }

  bool result = false;
  EVP_MD_CTX ctx;

  *signature = reinterpret_cast<unsigned char *>(
                 smalloc(EVP_PKEY_size(private_key_)));
  EVP_MD_CTX_init(&ctx);
  if (EVP_SignInit(&ctx, EVP_sha1()) &&
      EVP_SignUpdate(&ctx, buffer, buffer_size) &&
      EVP_SignFinal(&ctx, *signature, signature_size, private_key_))
  {
    result = true;
  }
  EVP_MD_CTX_cleanup(&ctx);
  if (!result) {
    free(*signature);
    *signature_size = 0;
    *signature = NULL;
  }

  return result;
}


/**
 * Veryfies a signature against loaded certificate.
 *
 * \return True if signature is valid, false on error or otherwise
 */
bool Verify(const unsigned char *buffer, const unsigned buffer_size,
            const unsigned char *signature, const unsigned signature_size)
{
  if (!certificate_) return false;

  bool result = false;
  EVP_MD_CTX ctx;

  EVP_MD_CTX_init(&ctx);
  if (EVP_VerifyInit(&ctx, EVP_sha1()) &&
      EVP_VerifyUpdate(&ctx, buffer, buffer_size) &&
      EVP_VerifyFinal(&ctx, signature, signature_size,
                      X509_get_pubkey(certificate_)))
  {
    result = true;
  }
  EVP_MD_CTX_cleanup(&ctx);

  return result;
}


/**
 * Veryfies a signature against all loaded public keys.
 *
 * \return True if signature is valid with any public key, false on error or otherwise
 */
bool VerifyRsa(const unsigned char *buffer, const unsigned buffer_size,
               const unsigned char *signature, const unsigned signature_size)
{
  for (unsigned i = 0, s = public_keys_->size(); i < s; ++i) {
    if (buffer_size > (unsigned)RSA_size((*public_keys_)[i]))
      continue;

    unsigned char *to = (unsigned char *)smalloc(RSA_size((*public_keys_)[i]));
    unsigned char *from = (unsigned char *)smalloc(signature_size);
    memcpy(from, signature, signature_size);

    int size = RSA_public_decrypt(signature_size, from, to,
                                  (*public_keys_)[i], RSA_PKCS1_PADDING);
    free(from);
    if ((size >= 0) && (unsigned(size) == buffer_size) &&
        (memcmp(buffer, to, size) == 0))
    {
      free(to);
      return true;
    }

    free(to);
  }

  return false;
}


/**
 * Reads after skip bytes in memory, looks for a line break and saves
 * the rest into sig_buf, which will be allocated.
 */
bool ReadSignatureTail(const unsigned char *buffer, const unsigned buffer_size,
                       const unsigned skip_bytes,
                       unsigned char **signature, unsigned *signature_size)
{
  unsigned i;
  for (i = skip_bytes; i < buffer_size; ++i) {
    if (((char *)buffer)[i] == '\n') break;
  }
  i++;
  /* at least one byte after \n required */
  if (i >= buffer_size) {
    *signature = NULL;
    *signature_size = 0;
    return false;
  } else {
    *signature_size = buffer_size-i;
    *signature = reinterpret_cast<unsigned char *>(smalloc(*signature_size));
    memcpy(*signature, ((char *)buffer)+i, *signature_size);
    return true;
  }
}

}  // namespace signature
