/*-
 * Copyright (c) 2020, AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef CRYPTO_RTE_PMD_H

#define CRYPTO_RTE_PMD_H

#include <linux/xfrm.h>

int crypto_rte_setup(void);

void crypto_rte_shutdown(void);

int crypto_rte_set_session_parameters(struct crypto_session *ctx,
				      const struct xfrm_algo *algo_crypt,
				      const struct xfrm_algo_auth *algo_auth);

/*
 * Crypto devices to instantiate in descending order of priority.
 * Whenever there is a need to instantiate a crypto device, the
 * available devices/drivers are checked starting with the first in this
 * list.
 */
enum cryptodev_type {
	CRYPTODEV_MIN,
	CRYPTODEV_AESNI_GCM = CRYPTODEV_MIN,
	CRYPTODEV_AESNI_MB,
	CRYPTODEV_OPENSSL,
	CRYPTODEV_MAX
};

int crypto_rte_select_pmd_type(enum rte_crypto_cipher_algorithm cipher_algo,
			       enum rte_crypto_aead_algorithm aead_algo,
			       enum cryptodev_type *dev_type,
			       bool *setup_openssl);

#endif
