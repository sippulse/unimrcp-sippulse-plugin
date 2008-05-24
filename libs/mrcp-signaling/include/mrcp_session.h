/*
 * Copyright 2008 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __MRCP_SESSION_H__
#define __MRCP_SESSION_H__

/**
 * @file mrcp_session.h
 * @brief Abstract MRCP Session
 */ 

#include "mrcp_sig_types.h"
#include "apt_string.h"

APT_BEGIN_EXTERN_C

/** MRCP session methods vtable declaration */
typedef struct mrcp_session_method_vtable_t mrcp_session_method_vtable_t;
/** MRCP session events vtable declaration */
typedef struct mrcp_session_event_vtable_t mrcp_session_event_vtable_t;

/** MRCP session */
struct mrcp_session_t {
	/** Memory pool to allocate memory from */
	apr_pool_t *pool;
	/** Session identifier */
	apt_str_t   session_id;

	/** Virtual methods */
	const mrcp_session_method_vtable_t *method_vtable;
	/** Virtual events */
	const mrcp_session_event_vtable_t  *event_vtable;
};

/** MRCP session methods vtable */
struct mrcp_session_method_vtable_t {
	/** Offer local description to remote party */
	apt_bool_t (*offer)(mrcp_session_t *session);
	/** Answer to offer, by setting up local description according to the remote one */
	apt_bool_t (*answer)(mrcp_session_t *session);
	/** Terminate session */
	apt_bool_t (*terminate)(mrcp_session_t *session);

	/** Destroy session (session must be terminated prior to destruction) */
	apt_bool_t (*destroy)(mrcp_session_t *session);
};

/** MRCP session events vtable */
struct mrcp_session_event_vtable_t {
	/** Receive offer from remote party */
	apt_bool_t (*on_offer)(mrcp_session_t *session);
	/** Receive answer from remote party */
	apt_bool_t (*on_answer)(mrcp_session_t *session);
	/** On terminate session */
	apt_bool_t (*on_terminate)(mrcp_session_t *session);
};

APT_END_EXTERN_C

#endif /*__MRCP_SESSION_H__*/