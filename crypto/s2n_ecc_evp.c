/*
 * Copyright 2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "crypto/s2n_ecc_evp.h"

#include <openssl/ecdh.h>
#include <openssl/evp.h>
#include <stdint.h>

#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

#define TLS_EC_CURVE_TYPE_NAMED 3

DEFINE_POINTER_CLEANUP_FUNC(EVP_PKEY *, EVP_PKEY_free);
DEFINE_POINTER_CLEANUP_FUNC(EVP_PKEY_CTX *, EVP_PKEY_CTX_free);

#if !MODERN_EC_SUPPORTED
DEFINE_POINTER_CLEANUP_FUNC(EC_KEY *, EC_KEY_free);
DEFINE_POINTER_CLEANUP_FUNC(EC_POINT *, EC_POINT_free);
#endif

#if MODERN_EC_SUPPORTED
const struct s2n_ecc_named_curve s2n_ecc_curve_x25519 = {
    .iana_id = TLS_EC_CURVE_ECDH_X25519, 
    .libcrypto_nid = NID_X25519, 
    .name = "x25519", 
    .share_size = 32
};
#endif

const struct s2n_ecc_named_curve *const s2n_ecc_evp_supported_curves_list[] = {
    &s2n_ecc_curve_secp256r1,
    &s2n_ecc_curve_secp384r1,
#if MODERN_EC_SUPPORTED
    &s2n_ecc_curve_x25519,
#endif
};

const size_t s2n_ecc_evp_supported_curves_list_len = s2n_array_len(s2n_ecc_evp_supported_curves_list);

#if MODERN_EC_SUPPORTED
static int s2n_ecc_evp_generate_key_x25519(const struct s2n_ecc_named_curve *named_curve, EVP_PKEY **evp_pkey);
#else
static int s2n_ecc_evp_write_point_data_snug(const EC_POINT *point, const EC_GROUP *group, struct s2n_blob *out);
static int s2n_ecc_evp_calculate_point_length(const EC_POINT *point, const EC_GROUP *group, uint8_t *length);
static EC_POINT *s2n_ecc_evp_blob_to_point(struct s2n_blob *blob, const EC_KEY *ec_key);
#endif
static int s2n_ecc_evp_generate_key_nist_curves(const struct s2n_ecc_named_curve *named_curve, EVP_PKEY **evp_pkey);
static int s2n_ecc_evp_generate_own_key(const struct s2n_ecc_named_curve *named_curve, EVP_PKEY **evp_pkey);
static int s2n_ecc_evp_compute_shared_secret(EVP_PKEY *own_key, EVP_PKEY *peer_public, struct s2n_blob *shared_secret);

#if MODERN_EC_SUPPORTED
static int s2n_ecc_evp_generate_key_x25519(const struct s2n_ecc_named_curve *named_curve, EVP_PKEY **evp_pkey) {

    DEFER_CLEANUP(EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(named_curve->libcrypto_nid, NULL),
                  EVP_PKEY_CTX_free_pointer);
    S2N_ERROR_IF(pctx == NULL, S2N_ERR_ECDHE_GEN_KEY);

    GUARD_OSSL(EVP_PKEY_keygen_init(pctx), S2N_ERR_ECDHE_GEN_KEY);
    GUARD_OSSL(EVP_PKEY_keygen(pctx, evp_pkey), S2N_ERR_ECDHE_GEN_KEY);
    S2N_ERROR_IF(evp_pkey == NULL, S2N_ERR_ECDHE_GEN_KEY);

    return 0;
}
#endif

static int s2n_ecc_evp_generate_key_nist_curves(const struct s2n_ecc_named_curve *named_curve, EVP_PKEY **evp_pkey) {

    DEFER_CLEANUP(EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL), EVP_PKEY_CTX_free_pointer);
    S2N_ERROR_IF(pctx == NULL, S2N_ERR_ECDHE_GEN_KEY);

    GUARD_OSSL(EVP_PKEY_paramgen_init(pctx), S2N_ERR_ECDHE_GEN_KEY);
    GUARD_OSSL(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, named_curve->libcrypto_nid), S2N_ERR_ECDHE_GEN_KEY);

    DEFER_CLEANUP(EVP_PKEY *params = NULL, EVP_PKEY_free_pointer);
    GUARD_OSSL(EVP_PKEY_paramgen(pctx, &params), S2N_ERR_ECDHE_GEN_KEY);
    S2N_ERROR_IF(params == NULL, S2N_ERR_ECDHE_GEN_KEY);

    DEFER_CLEANUP(EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new(params, NULL), EVP_PKEY_CTX_free_pointer);
    S2N_ERROR_IF(kctx == NULL, S2N_ERR_ECDHE_GEN_KEY);

    GUARD_OSSL(EVP_PKEY_keygen_init(kctx), S2N_ERR_ECDHE_GEN_KEY);
    GUARD_OSSL(EVP_PKEY_keygen(kctx, evp_pkey), S2N_ERR_ECDHE_GEN_KEY);
    S2N_ERROR_IF(evp_pkey == NULL, S2N_ERR_ECDHE_GEN_KEY);

    return 0;
}

static int s2n_ecc_evp_generate_own_key(const struct s2n_ecc_named_curve *named_curve, EVP_PKEY **evp_pkey) {
#if MODERN_EC_SUPPORTED
    if (named_curve->libcrypto_nid == NID_X25519) {
        return s2n_ecc_evp_generate_key_x25519(named_curve, evp_pkey);
    }
#endif
    if (named_curve->libcrypto_nid == NID_X9_62_prime256v1 || named_curve->libcrypto_nid == NID_secp384r1) {
        return s2n_ecc_evp_generate_key_nist_curves(named_curve, evp_pkey);
    }
    S2N_ERROR(S2N_ERR_ECDHE_GEN_KEY);
}

static int s2n_ecc_evp_compute_shared_secret(EVP_PKEY *own_key, EVP_PKEY *peer_public, struct s2n_blob *shared_secret) {
    notnull_check(peer_public);
    notnull_check(own_key);

    size_t shared_secret_size;
    
    DEFER_CLEANUP(EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(own_key, NULL), EVP_PKEY_CTX_free_pointer);
    S2N_ERROR_IF(ctx == NULL, S2N_ERR_ECDHE_SHARED_SECRET);

    GUARD_OSSL(EVP_PKEY_derive_init(ctx), S2N_ERR_ECDHE_SHARED_SECRET);
    GUARD_OSSL(EVP_PKEY_derive_set_peer(ctx, peer_public), S2N_ERR_ECDHE_SHARED_SECRET);
    GUARD_OSSL(EVP_PKEY_derive(ctx, NULL, &shared_secret_size), S2N_ERR_ECDHE_SHARED_SECRET);
    GUARD(s2n_alloc(shared_secret, shared_secret_size));

    if (EVP_PKEY_derive(ctx, shared_secret->data, &shared_secret_size) != 1) {
        GUARD(s2n_free(shared_secret));
        S2N_ERROR(S2N_ERR_ECDHE_SHARED_SECRET);
    }

    return 0;
}

int s2n_ecc_evp_generate_ephemeral_key(struct s2n_ecc_evp_params *ecc_evp_params) {
    notnull_check(ecc_evp_params->negotiated_curve);
    S2N_ERROR_IF(s2n_ecc_evp_generate_own_key(ecc_evp_params->negotiated_curve, &ecc_evp_params->evp_pkey) != 0,
                 S2N_ERR_ECDHE_GEN_KEY);
    S2N_ERROR_IF(ecc_evp_params->evp_pkey == NULL, S2N_ERR_ECDHE_GEN_KEY);
    return 0;
}

int s2n_ecc_evp_compute_shared_secret_from_params(struct s2n_ecc_evp_params *private_ecc_evp_params,
                                                  struct s2n_ecc_evp_params *public_ecc_evp_params,
                                                  struct s2n_blob *shared_key) {
    notnull_check(private_ecc_evp_params->negotiated_curve);
    notnull_check(private_ecc_evp_params->evp_pkey);
    notnull_check(public_ecc_evp_params->negotiated_curve);
    notnull_check(public_ecc_evp_params->evp_pkey);
    S2N_ERROR_IF(private_ecc_evp_params->negotiated_curve->iana_id != public_ecc_evp_params->negotiated_curve->iana_id,
                 S2N_ERR_ECDHE_UNSUPPORTED_CURVE);
    GUARD(s2n_ecc_evp_compute_shared_secret(private_ecc_evp_params->evp_pkey, public_ecc_evp_params->evp_pkey,
                                            shared_key));
    return 0;
}

#if (!MODERN_EC_SUPPORTED)
static int s2n_ecc_evp_calculate_point_length(const EC_POINT *point, const EC_GROUP *group, uint8_t *length) {
    size_t ret = EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, NULL, 0, NULL);
    S2N_ERROR_IF(ret == 0, S2N_ERR_ECDHE_SERIALIZING);
    S2N_ERROR_IF(ret > UINT8_MAX, S2N_ERR_ECDHE_SERIALIZING);
    *length = (uint8_t)ret;
    return 0;
}

static int s2n_ecc_evp_write_point_data_snug(const EC_POINT *point, const EC_GROUP *group, struct s2n_blob *out) {
    size_t ret = EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, out->data, out->size, NULL);
    S2N_ERROR_IF(ret != out->size, S2N_ERR_ECDHE_SERIALIZING);
    return 0;
}

static EC_POINT *s2n_ecc_evp_blob_to_point(struct s2n_blob *blob, const EC_KEY *ec_key) {
    const EC_GROUP *group = EC_KEY_get0_group(ec_key);
    EC_POINT *point = EC_POINT_new(group);
    if (point == NULL) {
        S2N_ERROR_PTR(S2N_ERR_ECDHE_UNSUPPORTED_CURVE);
    }
    if (EC_POINT_oct2point(group, point, blob->data, blob->size, NULL) != 1) {
        EC_POINT_free(point);
        S2N_ERROR_PTR(S2N_ERR_BAD_MESSAGE);
    }
    return point;
}
#endif

int s2n_ecc_evp_read_params_point(struct s2n_stuffer *in, int point_size, struct s2n_blob *point_blob) {
    notnull_check(in);
    notnull_check(point_blob);
    gte_check(point_size, 0);

    /* Extract point from stuffer */
    point_blob->size = point_size;
    point_blob->data = s2n_stuffer_raw_read(in, point_size);
    notnull_check(point_blob->data);

    return 0;
}

int s2n_ecc_evp_read_params(struct s2n_stuffer *in, struct s2n_blob *data_to_verify,
                            struct s2n_ecdhe_raw_server_params *raw_server_ecc_params) {
    notnull_check(in);
    uint8_t curve_type;
    uint8_t point_length;

    /* Remember where we started reading the data */
    data_to_verify->data = s2n_stuffer_raw_read(in, 0);
    notnull_check(data_to_verify->data);

    /* Read the curve */
    GUARD(s2n_stuffer_read_uint8(in, &curve_type));
    S2N_ERROR_IF(curve_type != TLS_EC_CURVE_TYPE_NAMED, S2N_ERR_BAD_MESSAGE);
    raw_server_ecc_params->curve_blob.data =  s2n_stuffer_raw_read(in, 2);
    notnull_check(raw_server_ecc_params->curve_blob.data);
    raw_server_ecc_params->curve_blob.size = 2;

    /* Read the point */
    GUARD(s2n_stuffer_read_uint8(in, &point_length));

    GUARD(s2n_ecc_evp_read_params_point(in, point_length, &raw_server_ecc_params->point_blob));

    /* curve type (1) + iana (2) + key share size (1) + key share */
    data_to_verify->size = point_length + 4;

    return 0;
}

int s2n_ecc_evp_write_params_point(struct s2n_ecc_evp_params *ecc_evp_params, struct s2n_stuffer *out) {
    notnull_check(ecc_evp_params);
    notnull_check(ecc_evp_params->negotiated_curve);
    notnull_check(ecc_evp_params->evp_pkey);
    notnull_check(out);

#if MODERN_EC_SUPPORTED
    struct s2n_blob point_blob = {0};
    uint8_t *encoded_point = NULL;
    
    point_blob.data = s2n_stuffer_raw_write(out, ecc_evp_params->negotiated_curve->share_size);
    notnull_check(point_blob.data);

    size_t size = EVP_PKEY_get1_tls_encodedpoint(ecc_evp_params->evp_pkey, &encoded_point);
    S2N_ERROR_IF(size != ecc_evp_params->negotiated_curve->share_size, S2N_ERR_ECDHE_SERIALIZING);

    memcpy(point_blob.data, encoded_point, size);
    OPENSSL_free(encoded_point);
#else
    uint8_t point_len;
    struct s2n_blob point_blob = {0};

    DEFER_CLEANUP(EC_KEY *ec_key = EVP_PKEY_get1_EC_KEY(ecc_evp_params->evp_pkey), EC_KEY_free_pointer);
    S2N_ERROR_IF(ec_key == NULL, S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    const EC_POINT *point = EC_KEY_get0_public_key(ec_key);
    const EC_GROUP *group = EC_KEY_get0_group(ec_key);
    S2N_ERROR_IF(point == NULL || group == NULL, S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    GUARD(s2n_ecc_evp_calculate_point_length(point, group, &point_len));
    S2N_ERROR_IF(point_len != ecc_evp_params->negotiated_curve->share_size, S2N_ERR_ECDHE_SERIALIZING);
    point_blob.data = s2n_stuffer_raw_write(out, point_len);
    notnull_check(point_blob.data);
    point_blob.size = point_len;

    GUARD(s2n_ecc_evp_write_point_data_snug(point, group, &point_blob));
#endif
    return 0;
}

int s2n_ecc_evp_write_params(struct s2n_ecc_evp_params *ecc_evp_params, struct s2n_stuffer *out,
                             struct s2n_blob *written) {
    notnull_check(ecc_evp_params);
    notnull_check(ecc_evp_params->negotiated_curve);
    notnull_check(ecc_evp_params->evp_pkey);
    notnull_check(out);
    notnull_check(written);

    int key_share_size = ecc_evp_params->negotiated_curve->share_size;
    /* Remember where the written data starts */
    written->data = s2n_stuffer_raw_write(out, 0);
    notnull_check(written->data);

    GUARD(s2n_stuffer_write_uint8(out, TLS_EC_CURVE_TYPE_NAMED));
    GUARD(s2n_stuffer_write_uint16(out, ecc_evp_params->negotiated_curve->iana_id));
    GUARD(s2n_stuffer_write_uint8(out, key_share_size));

    GUARD(s2n_ecc_evp_write_params_point(ecc_evp_params, out));

    /* key share + key share size (1) + iana (2) + curve type (1) */
    written->size = key_share_size + 4;

    return written->size;
}

int s2n_ecc_evp_parse_params_point(struct s2n_blob *point_blob, struct s2n_ecc_evp_params *ecc_evp_params) {
    notnull_check(point_blob->data);
    notnull_check(ecc_evp_params->negotiated_curve);
    S2N_ERROR_IF(point_blob->size != ecc_evp_params->negotiated_curve->share_size, S2N_ERR_ECDHE_SERIALIZING);

    if (ecc_evp_params->evp_pkey == NULL) {
        ecc_evp_params->evp_pkey = EVP_PKEY_new();
    }

    S2N_ERROR_IF(ecc_evp_params->evp_pkey == NULL, S2N_ERR_BAD_MESSAGE);

#if MODERN_EC_SUPPORTED
    GUARD_OSSL(EVP_PKEY_set1_tls_encodedpoint(ecc_evp_params->evp_pkey, point_blob->data, point_blob->size),
               S2N_ERR_ECDHE_SERIALIZING);
#else
    /* Create a key to store the point */
    DEFER_CLEANUP(EC_KEY *ec_key = EC_KEY_new_by_curve_name(ecc_evp_params->negotiated_curve->libcrypto_nid),
                  EC_KEY_free_pointer);
    S2N_ERROR_IF(ec_key == NULL, S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    /* Parse and store the server public point */
    DEFER_CLEANUP(EC_POINT *point = s2n_ecc_evp_blob_to_point(point_blob, ec_key), EC_POINT_free_pointer);
    S2N_ERROR_IF(point == NULL, S2N_ERR_BAD_MESSAGE);

    /* Set the point as the public key */
    int success = EC_KEY_set_public_key(ec_key, point);

    GUARD_OSSL(EVP_PKEY_set1_EC_KEY(ecc_evp_params->evp_pkey,ec_key), S2N_ERR_ECDHE_SERIALIZING);

    /* EC_KEY_set_public_key returns 1 on success, 0 on failure */
    S2N_ERROR_IF(success == 0, S2N_ERR_BAD_MESSAGE);

#endif
    return 0;
}

int s2n_ecc_evp_parse_params(struct s2n_ecdhe_raw_server_params *raw_server_ecc_params,
                             struct s2n_ecc_evp_params *ecc_evp_params) {
    S2N_ERROR_IF(
        s2n_ecc_evp_find_supported_curve(&raw_server_ecc_params->curve_blob, &ecc_evp_params->negotiated_curve) != 0,
        S2N_ERR_ECDHE_UNSUPPORTED_CURVE);
    return s2n_ecc_evp_parse_params_point(&raw_server_ecc_params->point_blob, ecc_evp_params);
}

int s2n_ecc_evp_find_supported_curve(struct s2n_blob *iana_ids, const struct s2n_ecc_named_curve **found) {
    struct s2n_stuffer iana_ids_in = {0};

    GUARD(s2n_stuffer_init(&iana_ids_in, iana_ids));
    GUARD(s2n_stuffer_write(&iana_ids_in, iana_ids));
    for (int i = 0; i < s2n_ecc_evp_supported_curves_list_len; i++) {
        const struct s2n_ecc_named_curve *supported_curve = s2n_ecc_evp_supported_curves_list[i];
        for (int j = 0; j < iana_ids->size / 2; j++) {
            uint16_t iana_id;
            GUARD(s2n_stuffer_read_uint16(&iana_ids_in, &iana_id));
            if (supported_curve->iana_id == iana_id) {
                *found = supported_curve;
                return 0;
            }
        }
        GUARD(s2n_stuffer_reread(&iana_ids_in));
    }

    S2N_ERROR(S2N_ERR_ECDHE_UNSUPPORTED_CURVE);
}

int s2n_ecc_evp_generate_copy_params(struct s2n_ecc_evp_params *from_params, struct s2n_ecc_evp_params *to_params) {
    notnull_check(from_params->evp_pkey);
    notnull_check(from_params->negotiated_curve);
    notnull_check(to_params->negotiated_curve);
    S2N_ERROR_IF(from_params->negotiated_curve != to_params->negotiated_curve, S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    to_params->evp_pkey = EVP_PKEY_new();
    S2N_ERROR_IF(to_params->evp_pkey == NULL, S2N_ERR_ECDHE_GEN_KEY);

    /* Copy EVP_PKEY Paramaters */
    S2N_ERROR_IF(EVP_PKEY_copy_parameters(to_params->evp_pkey, from_params->evp_pkey) != 1, S2N_ERR_ECDHE_SERIALIZING);
    S2N_ERROR_IF(!EVP_PKEY_missing_parameters(to_params->evp_pkey) &&
                     !EVP_PKEY_cmp_parameters(from_params->evp_pkey, to_params->evp_pkey),
                 S2N_ERR_ECDHE_SERIALIZING);
    return 0;
}

int s2n_ecc_evp_params_free(struct s2n_ecc_evp_params *ecc_evp_params) {
    if (ecc_evp_params->evp_pkey != NULL) {
        EVP_PKEY_free(ecc_evp_params->evp_pkey);
        ecc_evp_params->evp_pkey = NULL;
    }
    return 0;
}
