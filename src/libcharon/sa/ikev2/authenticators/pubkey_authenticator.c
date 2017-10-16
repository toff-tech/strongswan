/*
 * Copyright (C) 2008-2017 Tobias Brunner
 * Copyright (C) 2005-2009 Martin Willi
 * Copyright (C) 2005 Jan Hutter
 * HSR Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "pubkey_authenticator.h"

#include <daemon.h>
#include <encoding/payloads/auth_payload.h>
#include <sa/ikev2/keymat_v2.h>
#include <asn1/asn1.h>
#include <asn1/oid.h>
#include <collections/array.h>

typedef struct private_pubkey_authenticator_t private_pubkey_authenticator_t;

/**
 * Private data of an pubkey_authenticator_t object.
 */
struct private_pubkey_authenticator_t {

	/**
	 * Public authenticator_t interface.
	 */
	pubkey_authenticator_t public;

	/**
	 * Assigned IKE_SA
	 */
	ike_sa_t *ike_sa;

	/**
	 * nonce to include in AUTH calculation
	 */
	chunk_t nonce;

	/**
	 * IKE_SA_INIT message data to include in AUTH calculation
	 */
	chunk_t ike_sa_init;

	/**
	 * Reserved bytes of ID payload
	 */
	char reserved[3];
};

/**
 * Parse authentication data used for Signature Authentication as per RFC 7427
 */
static bool parse_signature_auth_data(chunk_t *auth_data, key_type_t *key_type,
									  signature_params_t *params)
{
	chunk_t parameters = chunk_empty;
	uint8_t len;
	int oid;

	if (!auth_data->len)
	{
		return FALSE;
	}
	len = auth_data->ptr[0];
	*auth_data = chunk_skip(*auth_data, 1);
	oid = asn1_parse_algorithmIdentifier(*auth_data, 1, &parameters);
	params->scheme = signature_scheme_from_oid(oid);
	switch (params->scheme)
	{
		case SIGN_UNKNOWN:
			return FALSE;
		case SIGN_RSA_EMSA_PSS:
		{
			rsa_pss_params_t *pss = malloc_thing(rsa_pss_params_t);

			if (!rsa_pss_params_parse(parameters, 0, pss))
			{
				DBG1(DBG_IKE, "failed parsing RSASSA-PSS parameters");
				free(pss);
				return FALSE;
			}
			params->params = pss;
			break;
		}
		default:
			break;
	}
	*key_type = key_type_from_signature_scheme(params->scheme);
	*auth_data = chunk_skip(*auth_data, len);
	return TRUE;
}

/**
 * Build authentication data used for Signature Authentication as per RFC 7427
 */
static bool build_signature_auth_data(chunk_t *auth_data,
									  signature_params_t *params)
{
	chunk_t data, parameters = chunk_empty;
	uint8_t len;
	int oid;

	oid = signature_scheme_to_oid(params->scheme);
	if (oid == OID_UNKNOWN)
	{
		return FALSE;
	}
	if (params->scheme == SIGN_RSA_EMSA_PSS &&
		!rsa_pss_params_build(params->params, &parameters))
	{
		return FALSE;
	}
	if (parameters.len)
	{
		data = asn1_algorithmIdentifier_params(oid, parameters);
	}
	else
	{
		data = asn1_algorithmIdentifier(oid);
	}
	len = data.len;
	*auth_data = chunk_cat("cmm", chunk_from_thing(len), data, *auth_data);
	return TRUE;
}

/**
 * Selects possible signature schemes based on our configuration, the other
 * peer's capabilities and the private key
 */
static array_t *select_signature_schemes(keymat_v2_t *keymat,
									auth_cfg_t *auth, private_key_t *private)
{
	enumerator_t *enumerator;
	signature_scheme_t scheme;
	signature_params_t *config;
	auth_rule_t rule;
	key_type_t key_type;
	bool have_config = FALSE;
	array_t *selected;

	selected = array_create(0, 0);
	key_type = private->get_type(private);
	enumerator = auth->create_enumerator(auth);
	while (enumerator->enumerate(enumerator, &rule, &config))
	{
		if (rule != AUTH_RULE_IKE_SIGNATURE_SCHEME)
		{
			continue;
		}
		have_config = TRUE;
		if (key_type == key_type_from_signature_scheme(config->scheme) &&
			keymat->hash_algorithm_supported(keymat,
								hasher_from_signature_scheme(config->scheme,
															 config->params)))
		{
			array_insert(selected, ARRAY_TAIL, signature_params_clone(config));
		}
	}
	enumerator->destroy(enumerator);

	if (!have_config)
	{
		/* if no specific configuration, find schemes appropriate for the key
		 * and supported by the other peer */
		enumerator = signature_schemes_for_key(key_type,
											   private->get_keysize(private));
		while (enumerator->enumerate(enumerator, &config))
		{
			if (config->scheme == SIGN_RSA_EMSA_PSS &&
				!lib->settings->get_bool(lib->settings, "%s.rsa_pss", FALSE,
										 lib->ns))
			{
				continue;
			}
			if (keymat->hash_algorithm_supported(keymat,
								hasher_from_signature_scheme(config->scheme,
															 config->params)))
			{
				array_insert(selected, ARRAY_TAIL,
							 signature_params_clone(config));
			}
		}
		enumerator->destroy(enumerator);

		/* for RSA we tried at least SHA-512, also try other schemes */
		if (key_type == KEY_RSA)
		{
			signature_scheme_t schemes[] = {
				SIGN_RSA_EMSA_PKCS1_SHA2_384,
				SIGN_RSA_EMSA_PKCS1_SHA2_256,
			}, contained;
			bool found;
			int i, j;

			for (i = 0; i < countof(schemes); i++)
			{
				scheme = schemes[i];
				found = FALSE;
				for (j = 0; j < array_count(selected); j++)
				{
					array_get(selected, j, &contained);
					if (scheme == contained)
					{
						found = TRUE;
						break;
					}
				}
				if (!found && keymat->hash_algorithm_supported(keymat,
										hasher_from_signature_scheme(scheme,
																	 NULL)))
				{
					INIT(config,
						.scheme = scheme,
					)
					array_insert(selected, ARRAY_TAIL, config);
				}
			}
		}
	}
	return selected;
}

CALLBACK(destroy_scheme, void,
	signature_params_t *params, int idx, void *user)
{
	signature_params_destroy(params);
}

/**
 * Create a signature using RFC 7427 signature authentication
 */
static status_t sign_signature_auth(private_pubkey_authenticator_t *this,
							auth_cfg_t *auth, private_key_t *private,
							identification_t *id, chunk_t *auth_data)
{
	enumerator_t *enumerator;
	keymat_v2_t *keymat;
	signature_scheme_t scheme = SIGN_UNKNOWN;
	signature_params_t *params;
	array_t *schemes;
	chunk_t octets = chunk_empty;
	status_t status = FAILED;

	keymat = (keymat_v2_t*)this->ike_sa->get_keymat(this->ike_sa);
	schemes = select_signature_schemes(keymat, auth, private);
	if (!array_count(schemes))
	{
		DBG1(DBG_IKE, "no common hash algorithm found to create signature "
			 "with %N key", key_type_names, private->get_type(private));
		array_destroy(schemes);
		return FAILED;
	}

	if (keymat->get_auth_octets(keymat, FALSE, this->ike_sa_init,
								this->nonce, id, this->reserved, &octets,
								schemes))
	{
		enumerator = array_create_enumerator(schemes);
		while (enumerator->enumerate(enumerator, &params))
		{
			scheme = params->scheme;
			if (private->sign(private, scheme, params->params, octets,
							  auth_data) &&
				build_signature_auth_data(auth_data, params))
			{
				status = SUCCESS;
				break;
			}
			else
			{
				DBG2(DBG_IKE, "unable to create %N signature for %N key",
					 signature_scheme_names, scheme, key_type_names,
					 private->get_type(private));
			}
		}
		enumerator->destroy(enumerator);
	}
	DBG1(DBG_IKE, "authentication of '%Y' (myself) with %N %s", id,
		 signature_scheme_names, scheme,
		 status == SUCCESS ? "successful" : "failed");
	array_destroy_function(schemes, destroy_scheme, NULL);
	chunk_free(&octets);
	return status;
}

/**
 * Get the auth octets and the signature scheme (in case it is changed by the
 * keymat).
 */
static bool get_auth_octets_scheme(private_pubkey_authenticator_t *this,
								   bool verify, identification_t *id,
								   chunk_t *octets, signature_params_t **scheme)
{
	keymat_v2_t *keymat;
	array_t *schemes;
	bool success = FALSE;

	schemes = array_create(0, 0);
	array_insert(schemes, ARRAY_TAIL, *scheme);

	keymat = (keymat_v2_t*)this->ike_sa->get_keymat(this->ike_sa);
	if (keymat->get_auth_octets(keymat, verify, this->ike_sa_init, this->nonce,
								id, this->reserved, octets, schemes) &&
		array_remove(schemes, 0, scheme))
	{
		success = TRUE;
	}
	else
	{
		*scheme = NULL;
	}
	array_destroy_function(schemes, destroy_scheme, NULL);
	return success;
}

/**
 * Create a classic IKEv2 signature
 */
static status_t sign_classic(private_pubkey_authenticator_t *this,
							 auth_cfg_t *auth, private_key_t *private,
							 identification_t *id, auth_method_t *auth_method,
							 chunk_t *auth_data)
{
	signature_scheme_t scheme;
	signature_params_t *params;
	chunk_t octets = chunk_empty;
	status_t status = FAILED;

	switch (private->get_type(private))
	{
		case KEY_RSA:
			scheme = SIGN_RSA_EMSA_PKCS1_SHA1;
			*auth_method = AUTH_RSA;
			break;
		case KEY_ECDSA:
			/* deduct the signature scheme from the keysize */
			switch (private->get_keysize(private))
			{
				case 256:
					scheme = SIGN_ECDSA_256;
					*auth_method = AUTH_ECDSA_256;
					break;
				case 384:
					scheme = SIGN_ECDSA_384;
					*auth_method = AUTH_ECDSA_384;
					break;
				case 521:
					scheme = SIGN_ECDSA_521;
					*auth_method = AUTH_ECDSA_521;
					break;
				default:
					DBG1(DBG_IKE, "%d bit ECDSA private key size not supported",
						 private->get_keysize(private));
					return FAILED;
			}
			break;
		default:
			DBG1(DBG_IKE, "private key of type %N not supported",
				 key_type_names, private->get_type(private));
			return FAILED;
	}

	INIT(params,
		.scheme = scheme,
	);
	if (get_auth_octets_scheme(this, FALSE, id, &octets, &params) &&
		private->sign(private, params->scheme, NULL, octets, auth_data))
	{
		status = SUCCESS;
	}
	if (params)
	{
		signature_params_destroy(params);
	}
	DBG1(DBG_IKE, "authentication of '%Y' (myself) with %N %s", id,
		 auth_method_names, *auth_method,
		 status == SUCCESS ? "successful" : "failed");
	chunk_free(&octets);
	return status;
}

METHOD(authenticator_t, build, status_t,
	private_pubkey_authenticator_t *this, message_t *message)
{
	private_key_t *private;
	identification_t *id;
	auth_cfg_t *auth;
	chunk_t auth_data;
	status_t status;
	auth_payload_t *auth_payload;
	auth_method_t auth_method = AUTH_NONE;

	id = this->ike_sa->get_my_id(this->ike_sa);
	auth = this->ike_sa->get_auth_cfg(this->ike_sa, TRUE);
	private = lib->credmgr->get_private(lib->credmgr, KEY_ANY, id, auth);
	if (!private)
	{
		DBG1(DBG_IKE, "no private key found for '%Y'", id);
		return NOT_FOUND;
	}

	if (this->ike_sa->supports_extension(this->ike_sa, EXT_SIGNATURE_AUTH))
	{
		auth_method = AUTH_DS;
		status = sign_signature_auth(this, auth, private, id, &auth_data);
	}
	else
	{
		status = sign_classic(this, auth, private, id, &auth_method,
							  &auth_data);
	}
	private->destroy(private);

	if (status == SUCCESS)
	{
		auth_payload = auth_payload_create();
		auth_payload->set_auth_method(auth_payload, auth_method);
		auth_payload->set_data(auth_payload, auth_data);
		chunk_free(&auth_data);
		message->add_payload(message, (payload_t*)auth_payload);
	}
	return status;
}

METHOD(authenticator_t, process, status_t,
	private_pubkey_authenticator_t *this, message_t *message)
{
	public_key_t *public;
	auth_method_t auth_method;
	auth_payload_t *auth_payload;
	chunk_t auth_data, octets;
	identification_t *id;
	auth_cfg_t *auth, *current_auth;
	enumerator_t *enumerator;
	key_type_t key_type = KEY_ECDSA;
	signature_params_t *params;
	status_t status = NOT_FOUND;
	const char *reason = "unsupported";
	bool online;

	auth_payload = (auth_payload_t*)message->get_payload(message, PLV2_AUTH);
	if (!auth_payload)
	{
		return FAILED;
	}
	INIT(params);
	auth_method = auth_payload->get_auth_method(auth_payload);
	auth_data = auth_payload->get_data(auth_payload);
	switch (auth_method)
	{
		case AUTH_RSA:
			key_type = KEY_RSA;
			params->scheme = SIGN_RSA_EMSA_PKCS1_SHA1;
			break;
		case AUTH_ECDSA_256:
			params->scheme = SIGN_ECDSA_256;
			break;
		case AUTH_ECDSA_384:
			params->scheme = SIGN_ECDSA_384;
			break;
		case AUTH_ECDSA_521:
			params->scheme = SIGN_ECDSA_521;
			break;
		case AUTH_DS:
			if (parse_signature_auth_data(&auth_data, &key_type, params))
			{
				break;
			}
			reason = "payload invalid";
			/* fall-through */
		default:
			DBG1(DBG_IKE, "%N authentication %s", auth_method_names,
				 auth_method, reason);
			signature_params_destroy(params);
			return INVALID_ARG;
	}
	id = this->ike_sa->get_other_id(this->ike_sa);
	if (!get_auth_octets_scheme(this, TRUE, id, &octets, &params))
	{
		return FAILED;
	}
	auth = this->ike_sa->get_auth_cfg(this->ike_sa, FALSE);
	online = !this->ike_sa->has_condition(this->ike_sa,
										  COND_ONLINE_VALIDATION_SUSPENDED);
	enumerator = lib->credmgr->create_public_enumerator(lib->credmgr,
													key_type, id, auth, online);
	while (enumerator->enumerate(enumerator, &public, &current_auth))
	{
		if (public->verify(public, params->scheme, params->params, octets,
						   auth_data))
		{
			DBG1(DBG_IKE, "authentication of '%Y' with %N successful", id,
				 auth_method == AUTH_DS ? signature_scheme_names : auth_method_names,
				 auth_method == AUTH_DS ? params->scheme : auth_method);
			status = SUCCESS;
			auth->merge(auth, current_auth, FALSE);
			auth->add(auth, AUTH_RULE_AUTH_CLASS, AUTH_CLASS_PUBKEY);
			auth->add(auth, AUTH_RULE_IKE_SIGNATURE_SCHEME,
					  signature_params_clone(params));
			if (!online)
			{
				auth->add(auth, AUTH_RULE_CERT_VALIDATION_SUSPENDED, TRUE);
			}
			break;
		}
		else
		{
			status = FAILED;
			DBG1(DBG_IKE, "signature validation failed, looking for another key");
		}
	}
	enumerator->destroy(enumerator);
	chunk_free(&octets);
	signature_params_destroy(params);
	if (status == NOT_FOUND)
	{
		DBG1(DBG_IKE, "no trusted %N public key found for '%Y'",
			 key_type_names, key_type, id);
	}
	return status;
}

METHOD(authenticator_t, destroy, void,
	private_pubkey_authenticator_t *this)
{
	free(this);
}

/*
 * Described in header.
 */
pubkey_authenticator_t *pubkey_authenticator_create_builder(ike_sa_t *ike_sa,
									chunk_t received_nonce, chunk_t sent_init,
									char reserved[3])
{
	private_pubkey_authenticator_t *this;

	INIT(this,
		.public = {
			.authenticator = {
				.build = _build,
				.process = (void*)return_failed,
				.is_mutual = (void*)return_false,
				.destroy = _destroy,
			},
		},
		.ike_sa = ike_sa,
		.ike_sa_init = sent_init,
		.nonce = received_nonce,
	);
	memcpy(this->reserved, reserved, sizeof(this->reserved));

	return &this->public;
}

/*
 * Described in header.
 */
pubkey_authenticator_t *pubkey_authenticator_create_verifier(ike_sa_t *ike_sa,
									chunk_t sent_nonce, chunk_t received_init,
									char reserved[3])
{
	private_pubkey_authenticator_t *this;

	INIT(this,
		.public = {
			.authenticator = {
				.build = (void*)return_failed,
				.process = _process,
				.is_mutual = (void*)return_false,
				.destroy = _destroy,
			},
		},
		.ike_sa = ike_sa,
		.ike_sa_init = received_init,
		.nonce = sent_nonce,
	);
	memcpy(this->reserved, reserved, sizeof(this->reserved));

	return &this->public;
}
