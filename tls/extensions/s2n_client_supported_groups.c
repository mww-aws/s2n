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

#include <sys/param.h>
#include <stdint.h>

#include "tls/extensions/s2n_client_supported_groups.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"

#include "utils/s2n_safety.h"

int s2n_extensions_client_supported_groups_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    int ec_curves_count = s2n_array_len(s2n_ecc_supported_curves);
    GUARD(s2n_stuffer_write_uint16(out, TLS_EXTENSION_SUPPORTED_GROUPS));
    GUARD(s2n_stuffer_write_uint16(out, 2 + ec_curves_count * 2));
    /* Curve list len */
    GUARD(s2n_stuffer_write_uint16(out, ec_curves_count * 2));
    /* Curve list */
    for (int i = 0; i < ec_curves_count; i++) {
        GUARD(s2n_stuffer_write_uint16(out, s2n_ecc_supported_curves[i]->iana_id));
    }

    GUARD(s2n_stuffer_write_uint16(out, TLS_EXTENSION_EC_POINT_FORMATS));
    GUARD(s2n_stuffer_write_uint16(out, 2));
    /* Point format list len */
    GUARD(s2n_stuffer_write_uint8(out, 1));
    /* Only allow uncompressed format */
    GUARD(s2n_stuffer_write_uint8(out, 0));
    
    return 0;
}
