/*
 * Copyright (c) 2020-2022, STMicroelectronics - All Rights Reserved
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <endian.h>
#include <errno.h>

#include <mbedtls/asn1.h>
#include <mbedtls/md.h>
#include <mbedtls/oid.h>
#include <mbedtls/platform.h>
#include <mbedtls/x509.h>

#include <common/debug.h>
#include <drivers/auth/crypto_mod.h>
#include <drivers/io/io_storage.h>
#include <drivers/st/bsec.h>
#include <drivers/st/stm32_hash.h>
#if STM32MP13
#include <drivers/st/stm32_pka.h>
#include <drivers/st/stm32_rng.h>
#include <drivers/st/stm32_saes.h>
#endif
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <plat/common/platform.h>
#include <tools_share/firmware_encrypted.h>

#include <platform_def.h>

#define CRYPTO_HASH_MAX_SIZE	32U
#define CRYPTO_SIGN_MAX_SIZE	64U
#define CRYPTO_PUBKEY_MAX_SIZE	64U
#define CRYPTO_MAX_TAG_SIZE	16U

#if STM32MP15
struct stm32mp_auth_ops {
	uint32_t (*verify_signature)(uint8_t *hash_in, uint8_t *pubkey_in,
				     uint8_t *signature, uint32_t ecc_algo);
};

static struct stm32mp_auth_ops auth_ops;

#endif

static void crypto_lib_init(void)
{
#if STM32MP15
	boot_api_context_t *boot_context =
		(boot_api_context_t *)stm32mp_get_boot_ctx_address();
#endif

	int ret;

	if (!stm32mp_is_auth_supported()) {
		return;
	}

#if STM32MP13
	if (stm32_rng_init() != 0) {
		panic();
	}

	if (stm32_pka_init() != 0) {
		panic();
	}
#ifndef DECRYPTION_SUPPORT_none
	if (stm32_saes_driver_init() != 0) {
		panic();
	}
#endif
#endif

#if STM32MP15
	auth_ops.verify_signature =
		boot_context->bootrom_ecdsa_verify_signature;
#endif

	ret = stm32_hash_register();
	if (ret != 0) {
		ERROR("HASH init (%d)\n", ret);
		panic();
	}
}

int get_plain_pk_from_asn1(void *pk_ptr, unsigned int pk_len, void **plain_pk,
			   unsigned int *len, int *pk_alg)
{
	int ret;
	mbedtls_pk_context mbedtls_pk = {0};
	unsigned char *p, *end;
	mbedtls_asn1_buf alg_params = {0};
	mbedtls_asn1_buf alg_oid = {0};

	*plain_pk = NULL;
	*len = 0U;

	/* Parse the public key */
	mbedtls_pk_init(&mbedtls_pk);
	p = (unsigned char *)pk_ptr;
	end = (unsigned char *)(p + pk_len);

	ret =  mbedtls_asn1_get_tag(&p, end, len,
				    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
	if (ret != 0) {
		return -EINVAL;
	}

	end = p + *len;
	ret = mbedtls_asn1_get_alg(&p, end, &alg_oid, &alg_params);
	if (ret != 0) {
		VERBOSE("%s: mbedtls_asn1_get_alg (%d)\n", __func__, ret);
		return -EINVAL;
	}

	if (pk_alg != NULL) {
		if ((strlen(MBEDTLS_OID_EC_GRP_SECP256R1) == alg_params.len) &&
		    (memcmp(MBEDTLS_OID_EC_GRP_SECP256R1, alg_params.p, alg_params.len) == 0)) {
			*pk_alg = BOOT_API_ECDSA_ALGO_TYPE_P256NIST;
		} else {
			*pk_alg = BOOT_API_ECDSA_ALGO_TYPE_BRAINPOOL256;
		}
	}

	ret = mbedtls_asn1_get_bitstring_null(&p, end, len);
	if (ret != 0) {
		VERBOSE("%s: mbedtls_asn1_get_bitstring_null (%d)\n", __func__, ret);
		return -EINVAL;
	}

	/* we remove the ident (0x04) first byte.  */
	if ((*len < 1U) || (p[0] !=  MBEDTLS_ASN1_OCTET_STRING)) {
		VERBOSE("%s: not expected len or tag\n", __func__);
		return -EINVAL;
	}

	*len = *len - 1U;
	*plain_pk = p + 1U;

	return 0;
}

#if STM32MP13
static uint32_t verify_signature(uint8_t *hash_in, uint8_t *pubkey_in,
				 uint8_t *signature, uint32_t ecc_algo)
{
	int ret;
	enum stm32_pka_ecdsa_curve_id cid;

	switch (ecc_algo) {
	case BOOT_API_ECDSA_ALGO_TYPE_P256NIST:
#if PKA_USE_NIST_P256
		cid = PKA_NIST_P256;
		break;
#else
		WARN("%s nist_p256 requested but not included\n", __func__);
		return CRYPTO_ERR_SIGNATURE;
#endif
	case BOOT_API_ECDSA_ALGO_TYPE_BRAINPOOL256:
#if PKA_USE_BRAINPOOL_P256T1
		cid = PKA_BRAINPOOL_P256T1;
		break;
#else
		WARN("%s brainpool_p256t1 requested but not included\n", __func__);
		return CRYPTO_ERR_SIGNATURE;
#endif
	default:
		WARN("%s unexpected ecc_algo(%d)\n", __func__, ecc_algo);
		return CRYPTO_ERR_SIGNATURE;
	}

	ret = stm32_pka_ecdsa_verif(hash_in,
				    BOOT_API_SHA256_DIGEST_SIZE_IN_BYTES,
				    signature, BOOT_API_ECDSA_SIGNATURE_LEN_IN_BYTES / 2U,
				    signature + BOOT_API_ECDSA_SIGNATURE_LEN_IN_BYTES / 2U,
				    BOOT_API_ECDSA_SIGNATURE_LEN_IN_BYTES / 2U,
				    pubkey_in, BOOT_API_ECDSA_PUB_KEY_LEN_IN_BYTES / 2U,
				    pubkey_in + BOOT_API_ECDSA_PUB_KEY_LEN_IN_BYTES / 2U,
				    BOOT_API_ECDSA_PUB_KEY_LEN_IN_BYTES / 2U, cid);
	if (ret < 0) {
		return CRYPTO_ERR_SIGNATURE;
	}

	return 0;
}

int plat_get_hashed_pk(void *full_pk_ptr, unsigned int full_pk_len,
		       void **hashed_pk_ptr, unsigned int *hashed_pk_len)
{
	static uint8_t st_pk[CRYPTO_PUBKEY_MAX_SIZE + sizeof(uint32_t)];
	int ret;
	void *plain_pk;
	unsigned int len;
	int curve_id;
	uint32_t cid;

	ret = get_plain_pk_from_asn1(full_pk_ptr, full_pk_len, &plain_pk, &len, &curve_id);
	if ((ret != 0) || (len > CRYPTO_PUBKEY_MAX_SIZE))  {
		return -EINVAL;
	}

	cid = curve_id; /* we want value of curve_id (1 or 2) in a uint32_t */

	memcpy(st_pk, &cid, sizeof(cid));
	memcpy(st_pk + sizeof(cid), plain_pk, len);

	*hashed_pk_ptr = st_pk;
	*hashed_pk_len = len + sizeof(cid);

	return 0;
}
#endif

#if STM32MP15
uint32_t verify_signature(uint8_t *hash_in, uint8_t *pubkey_in,
			  uint8_t *signature, uint32_t ecc_algo)
{
	int ret;

	ret = mmap_add_dynamic_region(STM32MP_ROM_BASE, STM32MP_ROM_BASE,
				      STM32MP_ROM_SIZE_2MB_ALIGNED, MT_CODE | MT_SECURE);
	if (ret != 0) {
		VERBOSE("%s: mmap_add_dynamic_region (%d)\n", __func__, ret);
		return CRYPTO_ERR_SIGNATURE;
	}

	ret = auth_ops.verify_signature(hash_in, pubkey_in, signature, ecc_algo);

	if (ret != BOOT_API_RETURN_OK) {
		VERBOSE("%s: auth_ops.verify_sign (%d)\n", __func__, ret);
		ret = CRYPTO_ERR_SIGNATURE;
	} else {
		ret = 0;
	}

	mmap_remove_dynamic_region(STM32MP_ROM_BASE, STM32MP_ROM_SIZE_2MB_ALIGNED);

	return ret;
}

int plat_get_hashed_pk(void *full_pk_ptr, unsigned int full_pk_len,
		       void **hashed_pk_ptr, unsigned int *hashed_pk_len)
{
	return get_plain_pk_from_asn1(full_pk_ptr, full_pk_len, hashed_pk_ptr, hashed_pk_len, NULL);
}
#endif

static int get_plain_digest_from_asn1(void *digest_ptr, unsigned int digest_len,
				      uint8_t **out, size_t *out_len, mbedtls_md_type_t *md_alg)
{
	int ret;
	mbedtls_asn1_buf hash_oid, params;
	size_t len;
	unsigned char *p, *end;

	*out = NULL;
	*out_len = 0U;

	/* Digest info should be an MBEDTLS_ASN1_SEQUENCE */
	p = (unsigned char *)digest_ptr;
	end = p + digest_len;
	ret = mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED |
				   MBEDTLS_ASN1_SEQUENCE);
	if (ret != 0) {
		return ret;
	}

	/* Get the hash algorithm */
	ret = mbedtls_asn1_get_alg(&p, end, &hash_oid, &params);
	if (ret != 0) {
		return ret;
	}

	ret = mbedtls_oid_get_md_alg(&hash_oid, md_alg);
	if (ret != 0) {
		return ret;
	}

	ret = mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OCTET_STRING);
	if (ret != 0) {
		return ret;
	}

	/* Length of hash must match the algorithm's size */
	if (len != BOOT_API_SHA256_DIGEST_SIZE_IN_BYTES) {
		return -1;
	}

	*out = p;
	*out_len = len;

	return 0;
}

static int crypto_verify_signature(void *data_ptr, unsigned int data_len,
				   void *sig_ptr, unsigned int sig_len,
				   void *sig_alg, unsigned int sig_alg_len,
				   void *pk_ptr, unsigned int pk_len)
{
	uint8_t image_hash[CRYPTO_HASH_MAX_SIZE] = {0};
	uint8_t sig[CRYPTO_SIGN_MAX_SIZE];
	uint8_t my_pk[CRYPTO_PUBKEY_MAX_SIZE];
	int ret;
	size_t len;
	mbedtls_asn1_sequence seq;
	unsigned char *p, *end;
	int curve_id;
	mbedtls_asn1_buf sig_oid, sig_params;
	mbedtls_md_type_t md_alg;
	mbedtls_pk_type_t pk_alg;

	/* Get pointers to signature OID and parameters */
	p = (unsigned char *)sig_alg;
	end = (unsigned char *)(p + sig_alg_len);
	ret = mbedtls_asn1_get_alg(&p, end, &sig_oid, &sig_params);
	if (ret != 0) {
		VERBOSE("%s: mbedtls_asn1_get_alg (%d)\n", __func__, ret);
		return CRYPTO_ERR_SIGNATURE;
	}

	/* Get the actual signature algorithm (MD + PK) */
	ret = mbedtls_oid_get_sig_alg(&sig_oid, &md_alg, &pk_alg);
	if (ret != 0) {
		VERBOSE("%s: mbedtls_oid_get_sig_alg (%d)\n", __func__, ret);
		return CRYPTO_ERR_SIGNATURE;
	}

	if ((md_alg != MBEDTLS_MD_SHA256) || (pk_alg != MBEDTLS_PK_ECDSA)) {
		VERBOSE("%s: md_alg=%d pk_alg=%d\n", __func__, md_alg, pk_alg);
		return CRYPTO_ERR_SIGNATURE;
	}

	ret = get_plain_pk_from_asn1(pk_ptr, pk_len, &pk_ptr, &pk_len, &curve_id);
	if (ret != 0) {
		VERBOSE("%s: get_plain_pk_from_asn1 (%d)\n", __func__, ret);
		return CRYPTO_ERR_SIGNATURE;
	}

	/* We expect a known pk_len */
	if (pk_len != sizeof(my_pk)) {
		VERBOSE("%s: pk_len=%d sizeof(my_pk)=%d)\n", __func__, pk_len, sizeof(my_pk));
		return CRYPTO_ERR_SIGNATURE;
	}

	/* Need to copy as auth_ops.verify_signature
	 * expects aligned public key.
	 */
	memcpy(my_pk, pk_ptr, sizeof(my_pk));

	/* Get the signature (bitstring) */
	p = (unsigned char *)sig_ptr;
	end = (unsigned char *)(p + sig_len);
	ret = mbedtls_asn1_get_bitstring_null(&p, end, &len);
	if (ret != 0) {
		VERBOSE("%s: mbedtls_asn1_get_bitstring_null (%d)\n", __func__, ret);
		return CRYPTO_ERR_SIGNATURE;
	}

	/* Get r and s from sequence */
	ret = mbedtls_asn1_get_sequence_of(&p, end, &seq, MBEDTLS_ASN1_INTEGER);
	if (ret != 0) {
		VERBOSE("%s: mbedtls_asn1_get_sequence_of (%d)\n", __func__, ret);
		return CRYPTO_ERR_SIGNATURE;
	}

	/* We expect only 2 integers (r and s) from the sequence */
	if (seq.next->next != NULL) {
		mbedtls_asn1_sequence *cur = seq.next;
		mbedtls_asn1_sequence *next;

		VERBOSE("%s: nb seq != 2\n", __func__);
		/* Free all the sequences */
		while (cur != NULL) {
			next = cur->next;
			mbedtls_free(cur);
			cur = next;
		}

		return CRYPTO_ERR_SIGNATURE;
	}

	/* Integer sequence may (sometime) start with 0x00 as MSB, but we can only
	 * manage exactly 2*32 bytes, we remove this higher byte
	 * if there are not 00, we will fail either.
	 */
	memcpy(sig, seq.buf.p + seq.buf.len - sizeof(sig) / 2U, sizeof(sig) / 2U);
	memcpy(sig +  sizeof(sig) / 2U,
	       seq.next->buf.p + seq.next->buf.len - sizeof(sig) / 2U,
	       sizeof(sig) / 2U);
	/* Need to free allocated 'next' in mbedtls_asn1_get_sequence_of */
	mbedtls_free(seq.next);

	/* Compute hash for the data covered by the signature */
	stm32_hash_init(HASH_SHA256);

	ret = stm32_hash_final_update((uint8_t *)data_ptr, data_len, image_hash);
	if (ret != 0) {
		VERBOSE("%s: stm32_hash_final_update (%d)\n", __func__, ret);
		return CRYPTO_ERR_SIGNATURE;
	}

	return verify_signature(image_hash, my_pk, sig, curve_id);
}

static int crypto_verify_hash(void *data_ptr, unsigned int data_len,
			      void *digest_info_ptr,
			      unsigned int digest_info_len)
{
	int ret;
	uint8_t calc_hash[BOOT_API_SHA256_DIGEST_SIZE_IN_BYTES];
	unsigned char *p;
	mbedtls_md_type_t md_alg;
	size_t len;

	/* we receive an asn1 encapsulated digest, we flatten it */
	ret = get_plain_digest_from_asn1(digest_info_ptr,
					 digest_info_len, &p, &len,
					 &md_alg);
	if ((ret != 0) || (md_alg != MBEDTLS_MD_SHA256) || (len != sizeof(calc_hash))) {
		return CRYPTO_ERR_HASH;
	}

	digest_info_ptr = p;
	digest_info_len = len;

	stm32_hash_init(HASH_SHA256);

	ret = stm32_hash_final_update(data_ptr, data_len, calc_hash);
	if (ret != 0) {
		VERBOSE("%s: hash failed\n", __func__);
		return CRYPTO_ERR_HASH;
	}

	ret = memcmp(calc_hash, digest_info_ptr, digest_info_len);
	if (ret != 0) {
		VERBOSE("%s: not expected digest\n", __func__);
		ret = CRYPTO_ERR_HASH;
	}

	return ret;
}

#if STM32MP13 && !defined(DECRYPTION_SUPPORT_none)
int derive_key(uint8_t *key, size_t *key_len, size_t len,
	       unsigned int *flags, const uint8_t *img_id, size_t img_id_len)
{
	size_t i, j;

	assert(*key_len >= 32U);

	/*
	 * Not a real derivation yet
	 *
	 * But we expect a 32 bytes key, and otp is only 16 bytes
	 *   => duplicate.
	 */
	for (i = 0U, j = len; j < 32U;
	     i += sizeof(uint32_t), j += sizeof(uint32_t)) {
		memcpy(key + j, key + i, sizeof(uint32_t));
	}

	*key_len = 32U;
	/* Variable 'key' store a real key */
	*flags = 0U;

	return 0;
}

int plat_get_enc_key_info(enum fw_enc_status_t fw_enc_status, uint8_t *key,
			  size_t *key_len, unsigned int *flags,
			  const uint8_t *img_id, size_t img_id_len)
{
	uint32_t otp_idx;
	uint32_t otp_len;
	size_t read_len;
	size_t i;

	if (fw_enc_status == FW_ENC_WITH_BSSK) {
		return -EINVAL;
	}

	if (stm32_get_otp_index(ENCKEY_OTP, &otp_idx, &otp_len) != 0) {
		VERBOSE("%s: get %s index error\n", __func__, ENCKEY_OTP);
		return -EINVAL;
	}

	if (otp_len > (*key_len * CHAR_BIT)) {
		VERBOSE("%s: length Error otp_len=%d key_len=%u\n", __func__,
			otp_len, *key_len * CHAR_BIT);
		return -EINVAL;
	}

	read_len = otp_len / CHAR_BIT;
	assert(read_len % sizeof(uint32_t) == 0);

	for (i = 0U; i < read_len / sizeof(uint32_t); i++) {
		uint32_t tmp;
		uint32_t otp_val;

		if (stm32_get_otp_value_from_idx(otp_idx + i, &otp_val) != 0) {
			VERBOSE("%s: unable to read from otp\n", __func__);
			return -EINVAL;
		}

		tmp = bswap32(otp_val);
		memcpy(key + i * sizeof(uint32_t), &tmp, sizeof(tmp));
	}

	/* Now we have the OTP values in key till read_len */

	if (derive_key(key, key_len, read_len, flags, img_id,
		       img_id_len) != 0) {
		return -EINVAL;
	}

	return 0;
}

static enum stm32_saes_key_selection select_key(unsigned int key_flags)
{
	if ((key_flags & ENC_KEY_IS_IDENTIFIER) != 0U) {
		panic();
	}

	/* Use the provided key buffer */
	return STM32_SAES_KEY_SOFT;
}

static int stm32_decrypt_aes_gcm(void *data, size_t data_len,
				 const void *key, unsigned int key_len,
				 unsigned int key_flags,
				 const void *iv, unsigned int iv_len,
				 const void *tag, unsigned int tag_len)
{
	int ret;
	struct stm32_saes_context ctx;
	unsigned char tag_buf[CRYPTO_MAX_TAG_SIZE];
	enum stm32_saes_key_selection key_mode;
	unsigned int diff, i;

	key_mode = select_key(key_flags);

	ret = stm32_saes_init(&ctx, true, STM32_SAES_MODE_GCM, key_mode, key,
			      key_len, iv, iv_len);
	if (ret != 0) {
		return CRYPTO_ERR_INIT;
	}

	ret = stm32_saes_update_assodata(&ctx, true, NULL, 0U);
	if (ret != 0) {
		return CRYPTO_ERR_DECRYPTION;
	}

	ret = stm32_saes_update_load(&ctx, true, data, data, data_len);
	if (ret != 0) {
		return CRYPTO_ERR_DECRYPTION;
	}

	ret = stm32_saes_final(&ctx, tag_buf, sizeof(tag_buf));
	if (ret != 0) {
		return CRYPTO_ERR_DECRYPTION;
	}

	/* Check tag in "constant-time" */
	for (diff = 0U, i = 0U; i < tag_len; i++) {
		diff |= ((const unsigned char *)tag)[i] ^ tag_buf[i];
	}

	if (diff != 0U) {
		return CRYPTO_ERR_DECRYPTION;
	}

	return CRYPTO_SUCCESS;
}

/*
 * Authenticated decryption of an image
 *
 */
static int crypto_auth_decrypt(enum crypto_dec_algo dec_algo, void *data_ptr, size_t len,
			       const void *key, unsigned int key_len, unsigned int key_flags,
			       const void *iv, unsigned int iv_len, const void *tag,
			       unsigned int tag_len)
{
	int rc;
	uint32_t real_iv[4];

	switch (dec_algo) {
	case CRYPTO_GCM_DECRYPT:
		/*
		 * GCM expect a Nonce
		 * The AES IV is the nonce (a uint32_t[3])
		 * then a counter (a uint32_t big endian)
		 * The counter starts at 2.
		 */
		memcpy(real_iv, iv, iv_len);
		real_iv[3] = htobe32(0x2U);

		rc = stm32_decrypt_aes_gcm(data_ptr, len, key, key_len, key_flags,
					   real_iv, sizeof(real_iv), tag, tag_len);
		if (rc != 0) {
			return rc;
		}
		break;
	default:
		return CRYPTO_ERR_DECRYPTION;
	}

	return CRYPTO_SUCCESS;
}

REGISTER_CRYPTO_LIB("stm32_crypto_lib",
		    crypto_lib_init,
		    crypto_verify_signature,
		    crypto_verify_hash,
		    crypto_auth_decrypt);

#else /* No decryption support */
REGISTER_CRYPTO_LIB("stm32_crypto_lib",
		    crypto_lib_init,
		    crypto_verify_signature,
		    crypto_verify_hash,
		    NULL);

#endif
