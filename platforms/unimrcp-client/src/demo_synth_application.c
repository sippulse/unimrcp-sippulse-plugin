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

#include "demo_application.h"
#include "mrcp_session.h"
#include "mrcp_message.h"
#include "mrcp_synth_resource.h"
#include "mrcp_generic_header.h"

static mrcp_message_t* synth_application_speak_message_create(demo_application_t *demo_application, mrcp_session_t *session, mrcp_channel_t *channel)
{
	const char text[] = 
		"<?xml version=\"1.0\"?>\r\n"
		"<speak>\r\n"
		"<paragraph>\r\n"
		"    <sentence>Hello World.</sentence>\r\n"
		"</paragraph>\r\n"
		"</speak>\r\n";

	mrcp_message_t *mrcp_message = mrcp_application_message_create(session,channel,SYNTHESIZER_SPEAK);
	if(mrcp_message) {
		mrcp_generic_header_t *generic_header;
		generic_header = mrcp_generic_header_prepare(mrcp_message);
		if(generic_header) {
			apt_string_assign(&generic_header->content_type,"application/synthesis+ssml",mrcp_message->pool);
			mrcp_generic_header_property_add(mrcp_message,GENERIC_HEADER_CONTENT_TYPE);
		}
		apt_string_assign(&mrcp_message->body,text,mrcp_message->pool);
	}
	return mrcp_message;
}


static apt_bool_t synth_application_run(demo_application_t *demo_application)
{
	mrcp_session_t *session = mrcp_application_session_create(demo_application->application,NULL);
	if(session) {
		mrcp_channel_t *channel = mrcp_application_channel_create(session,MRCP_SYNTHESIZER_RESOURCE,NULL,NULL);
		if(channel) {
			mrcp_message_t *mrcp_message;
			mrcp_application_channel_add(session,channel,NULL);

			mrcp_message = synth_application_speak_message_create(demo_application,session,channel);
			if(mrcp_message) {
				mrcp_application_message_send(session,channel,mrcp_message);
			}
		}
	}
	return TRUE;
}

static apt_bool_t synth_application_on_session_update(demo_application_t *demo_application, mrcp_session_t *session)
{
	return TRUE;
}

static apt_bool_t synth_application_on_session_terminate(demo_application_t *demo_application, mrcp_session_t *session)
{
	mrcp_application_session_destroy(session);
	return TRUE;
}

static apt_bool_t synth_application_on_channel_add(demo_application_t *demo_application, mrcp_session_t *session, mrcp_channel_t *channel, mpf_rtp_termination_descriptor_t *descriptor)
{
	return TRUE;
}

static apt_bool_t synth_application_on_message_receive(demo_application_t *demo_application, mrcp_session_t *session, mrcp_channel_t *channel, mrcp_message_t *message)
{
	mrcp_application_channel_remove(session,channel);
	return TRUE;
}

static apt_bool_t synth_application_on_channel_remove(demo_application_t *demo_application, mrcp_session_t *session, mrcp_channel_t *channel)
{
	mrcp_application_session_terminate(session);
	return TRUE;
}

static const demo_application_vtable_t synth_application_vtable = {
	synth_application_run,
	synth_application_on_session_update,
	synth_application_on_session_terminate,
	synth_application_on_channel_add,
	synth_application_on_channel_remove,
	synth_application_on_message_receive
};

demo_application_t* demo_synth_application_create(apr_pool_t *pool)
{
	demo_application_t *synth_application = apr_palloc(pool,sizeof(demo_application_t));
	synth_application->application = NULL;
	synth_application->framework = NULL;
	synth_application->vtable = &synth_application_vtable;
	return synth_application;
}