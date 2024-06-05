/*
 * Copyright 2008-2015 Arsen Chaloyan
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

/* 
 * Mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST implement a plugin/engine creator function
 *    with the exact signature and name (the main entry point)
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 */

#include "mrcp_synth_engine.h"
#include "apt_consumer_task.h"
#include "apt_log.h"
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <cjson/cJSON.h>


#define SYNTH_ENGINE_TASK_NAME "Sippulse Synth Engine"

typedef struct sippulse_synth_engine_t sippulse_synth_engine_t;
typedef struct sippulse_synth_channel_t sippulse_synth_channel_t;
typedef struct sippulse_synth_msg_t sippulse_synth_msg_t;

/** Declaration of synthesizer engine methods */
static apt_bool_t sippulse_synth_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t sippulse_synth_engine_open(mrcp_engine_t *engine);
static apt_bool_t sippulse_synth_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* sippulse_synth_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	sippulse_synth_engine_destroy,
	sippulse_synth_engine_open,
	sippulse_synth_engine_close,
	sippulse_synth_engine_channel_create
};


/** Declaration of synthesizer channel methods */
static apt_bool_t sippulse_synth_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t sippulse_synth_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t sippulse_synth_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t sippulse_synth_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	sippulse_synth_channel_destroy,
	sippulse_synth_channel_open,
	sippulse_synth_channel_close,
	sippulse_synth_channel_request_process
};

/** Declaration of synthesizer audio stream methods */
static apt_bool_t sippulse_synth_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t sippulse_synth_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t sippulse_synth_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t sippulse_synth_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	sippulse_synth_stream_destroy,
	sippulse_synth_stream_open,
	sippulse_synth_stream_close,
	sippulse_synth_stream_read,
	NULL,
	NULL,
	NULL,
	NULL
};

/** Declaration of sippulse synthesizer engine */
struct sippulse_synth_engine_t {
	apt_consumer_task_t    *task;
};

/** Declaration of sippulse synthesizer channel */
struct sippulse_synth_channel_t {
	/** Back pointer to engine */
	sippulse_synth_engine_t   *sippulse_engine;
	/** Engine channel base */
	mrcp_engine_channel_t *channel;
	/** Active (in-progress) speak request */
	mrcp_message_t        *speak_request;
	/** Pending stop response */
	mrcp_message_t        *stop_response;
	/** Estimated time to complete */
	apr_size_t             time_to_complete;
	/** Is paused */
	apt_bool_t             paused;
	//File URL
	char 			  	  *file_url;
	/** Speech source (used instead of actual synthesis) */
	FILE                  *audio_file;
};

typedef enum {
	SIPPULSE_SYNTH_MSG_OPEN_CHANNEL,
	SIPPULSE_SYNTH_MSG_CLOSE_CHANNEL,
	SIPPULSE_SYNTH_MSG_REQUEST_PROCESS
} sippulse_synth_msg_type_e;

/** Declaration of sippulse synthesizer task message */
struct sippulse_synth_msg_t {
	sippulse_synth_msg_type_e  type;
	mrcp_engine_channel_t *channel; 
	mrcp_message_t        *request;
};


static apt_bool_t sippulse_synth_msg_signal(sippulse_synth_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t sippulse_synth_msg_process(apt_task_t *task, apt_task_msg_t *msg);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="SYNTH-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(SYNTH_PLUGIN,"SYNTH-PLUGIN")

/** Use custom log source mark */
#define SYNTH_LOG_MARK   APT_LOG_MARK_DECLARE(SYNTH_PLUGIN)

/** GET the API from the enviroment variable SIPPULSE_API_KEY */
const char* get_api_key() {
    return getenv("SIPPULSE_API_KEY");
}

#define SIPPULSE_API_KEY get_api_key()

typedef struct {
    char *data; // Response data from the server
    size_t size; // Size of the response data
} http_response;

void swap_endian_16bit(void* data, size_t size) {
    uint8_t* bytes = (uint8_t*)data;
    for (size_t i = 0; i < size; i += 2) {
        // Swap the two bytes of each 16-bit sample
        uint8_t temp = bytes[i];
        bytes[i] = bytes[i + 1];
        bytes[i + 1] = temp;
    }
}

static apt_bool_t download_file(const char* file_url, const char* output_path) {
    CURL *curl;
    CURLcode res;
    FILE *file;
    
    curl = curl_easy_init();
    if(!curl) {
        apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "Failed to initialize curl");
        return FALSE;
    }

    file = fopen(output_path, "wb");
    if(!file) {
        apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "Failed to open file %s for writing", output_path);
        curl_easy_cleanup(curl);
        return FALSE;
    }

    curl_easy_setopt(curl, CURLOPT_URL, file_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL); // Use the default write function which writes to a FILE *
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file); // Write directly to the file
    
    res = curl_easy_perform(curl);
    if(res != CURLE_OK) {
        apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "curl_easy_perform() failed: %s", curl_easy_strerror(res));
        fclose(file);
        curl_easy_cleanup(curl);
        return FALSE;
    }

    fclose(file);
    curl_easy_cleanup(curl);
    return TRUE;
}

// Updated function prototype to accept an APR pool
static char* extract_stream_url_from_response(const char* response, apr_pool_t *pool) {
    cJSON *json = cJSON_Parse(response);
    if (!json) {
        apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "Failed to parse JSON response");
        return NULL;
    }

    cJSON *stream = cJSON_GetObjectItemCaseSensitive(json, "stream");
    char *stream_url = NULL;
    if (cJSON_IsString(stream) && (stream->valuestring != NULL)) {
        // Use the provided APR pool to duplicate the string
        stream_url = apr_pstrdup(pool, stream->valuestring);
    }

    cJSON_Delete(json);
    return stream_url;
}

// Callback function for writing received data from the HTTP request
static size_t write_response(void *contents, size_t size, size_t nmemb, http_response *res) {
    size_t real_size = size * nmemb;
    char *ptr = realloc(res->data, res->size + real_size + 1);
    if(ptr == NULL) return 0; // Out of memory

    res->data = ptr;
    memcpy(&(res->data[res->size]), contents, real_size);
    res->size += real_size;
    res->data[res->size] = 0; // Null-terminate the string

    return real_size;
}


void *perform_request(const char *voice, const char *text, sippulse_synth_channel_t *synth_channel) {
    CURL *curl;
    CURLcode res;
    http_response response = {.data = NULL, .size = 0};

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.sippulse.ai/v1/tts/azure");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        
        // Setup HTTP headers
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
		char auth_header[256];
		snprintf(auth_header, sizeof(auth_header), "api-key: %s", SIPPULSE_API_KEY);
		headers = curl_slist_append(headers, auth_header);
        
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // JSON data
        const char *json_data_template = "{\"text\": \"%s\", \"voice\": \"%s\", \"output_format\": 16}";
		char json_data_final[8192];
		snprintf(json_data_final, sizeof(json_data_final), json_data_template, text, voice);
		
		apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"JSON Data Template: %s", json_data_template);
		apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Voice: %s", voice);
		apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Text: %s", text);
		
		apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"JSON Data Final: %s", json_data_final);

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data_final);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response);

        // Perform the request
        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            // Here, you would handle the response, e.g., by passing it to another part of your plugin
            printf("Response from server: %s\n", response.data);
        }

		// Extract the stream URL to a char variable nopcm_url
		synth_channel->file_url = extract_stream_url_from_response(response.data, synth_channel->channel->pool);

		apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"File URL in perform request: %s", synth_channel->file_url);

		// Extract the stream URL from the response
		if(synth_channel->file_url == NULL) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"Failed to Extract Stream URL from Response");
		}	

        // Cleanup
        free(response.data);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    return NULL;
}

/** Create sippulse synthesizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	/* create sippulse engine */
	sippulse_synth_engine_t *sippulse_engine = apr_palloc(pool,sizeof(sippulse_synth_engine_t));
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	/* create task/thread to run sippulse engine in the context of this task */
	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(sippulse_synth_msg_t),pool);
	sippulse_engine->task = apt_consumer_task_create(sippulse_engine,msg_pool,pool);
	if(!sippulse_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(sippulse_engine->task);
	apt_task_name_set(task,SYNTH_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = sippulse_synth_msg_process;
	}

	/* create engine base */
	return mrcp_engine_create(
				MRCP_SYNTHESIZER_RESOURCE, /* MRCP resource identifier */
				sippulse_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

/** Destroy synthesizer engine */
static apt_bool_t sippulse_synth_engine_destroy(mrcp_engine_t *engine)
{
	sippulse_synth_engine_t *sippulse_engine = engine->obj;
	if(sippulse_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(sippulse_engine->task);
		apt_task_destroy(task);
		sippulse_engine->task = NULL;
	}
	return TRUE;
}

/** Open synthesizer engine */
static apt_bool_t sippulse_synth_engine_open(mrcp_engine_t *engine)
{
	sippulse_synth_engine_t *sippulse_engine = engine->obj;
	if(sippulse_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(sippulse_engine->task);
		apt_task_start(task);
	}
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close synthesizer engine */
static apt_bool_t sippulse_synth_engine_close(mrcp_engine_t *engine)
{
	sippulse_synth_engine_t *sippulse_engine = engine->obj;
	if(sippulse_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(sippulse_engine->task);
		apt_task_terminate(task,TRUE);
	}
	return mrcp_engine_close_respond(engine);
}

/** Create sippulse synthesizer channel derived from engine channel base */
static mrcp_engine_channel_t* sippulse_synth_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination; 

	/* create sippulse synth channel */
	sippulse_synth_channel_t *synth_channel = apr_palloc(pool,sizeof(sippulse_synth_channel_t));
	synth_channel->sippulse_engine = engine->obj;
	synth_channel->speak_request = NULL;
	synth_channel->stop_response = NULL;
	synth_channel->time_to_complete = 0;
	synth_channel->paused = FALSE;
	synth_channel->audio_file = NULL;
	
	capabilities = mpf_source_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
			"LPCM");

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			synth_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	synth_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			synth_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	return synth_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t sippulse_synth_channel_destroy(mrcp_engine_channel_t *channel)
{
	/* nothing to destroy */
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t sippulse_synth_channel_open(mrcp_engine_channel_t *channel)
{
	return sippulse_synth_msg_signal(SIPPULSE_SYNTH_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t sippulse_synth_channel_close(mrcp_engine_channel_t *channel)
{
	return sippulse_synth_msg_signal(SIPPULSE_SYNTH_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t sippulse_synth_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	return sippulse_synth_msg_signal(SIPPULSE_SYNTH_MSG_REQUEST_PROCESS,channel,request);
}

/** Process SPEAK request */
static apt_bool_t sippulse_synth_channel_speak(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	const char *file_path = NULL;
	sippulse_synth_channel_t *synth_channel = channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_source_stream_codec_get(channel);

	if(!descriptor) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		return FALSE;
	}

	// Extract voice and text from the request
	mrcp_synth_header_t *req_synth_header = mrcp_resource_header_get(request);
	const char *voice = req_synth_header->voice_param.name.buf;

	//get the text from the MRCP v2 synth request
	const char *text = request->body.buf;

	apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"SPEAK Request Received " APT_SIDRES_FMT,
		MRCP_MESSAGE_SIDRES(request));
	
	// Check if the response is valid
	if(response->start_line.status_code != MRCP_STATUS_CODE_SUCCESS) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Response from Server " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		return FALSE;
	}

	// Perform the HTTP request to the sippulse API
	perform_request(voice, text, synth_channel);

	// Check if the file URL is valid
	if(synth_channel->file_url == NULL) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"Failed to Get File URL from Server " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		return FALSE;
	} else {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"File URL in channel SPEAK: %s", synth_channel->file_url);
		char *file_name = strrchr(synth_channel->file_url, '/');
		if (file_name != NULL) {
			file_name++; // Move past the '/'
		} else {
			apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "Failed to extract file name from URL");
			return FALSE;
		}

		// Create the file path
		apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"File Name: %s", file_name);

		// Get the directory layout from the engine
		const apt_dir_layout_t *dir_layout = channel->engine->dir_layout;
		
		file_path = apt_vardir_filepath_get(dir_layout,file_name,channel->pool);
		apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"File Path: %s", file_path);

		// Download the file
		download_file(synth_channel->file_url, file_path);
	}
	
	apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"File Path: %s", file_path);
	if(file_path) {
		synth_channel->audio_file = fopen(file_path,"rb");
		synth_channel->time_to_complete = 0;
		if(synth_channel->audio_file) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set [%s] as Speech Source " APT_SIDRES_FMT,
				file_path,
				MRCP_MESSAGE_SIDRES(request));
		}
		else {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"No Speech Source [%s] Found " APT_SIDRES_FMT,
				file_path,
				MRCP_MESSAGE_SIDRES(request));
			/* calculate estimated time to complete */
			if(mrcp_generic_header_property_check(request,GENERIC_HEADER_CONTENT_LENGTH) == TRUE) {
				mrcp_generic_header_t *generic_header = mrcp_generic_header_get(request);
				if(generic_header) {
					synth_channel->time_to_complete = generic_header->content_length * 10; /* 10 msec per character */
				}
			}
		}
	} else {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"Failed to Get File Path " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
	}

	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	synth_channel->speak_request = request;
	return TRUE;
}

/** Process STOP request */
static apt_bool_t sippulse_synth_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	sippulse_synth_channel_t *synth_channel = channel->method_obj;
	/* store the request, make sure there is no more activity and only then send the response */
	synth_channel->stop_response = response;
	return TRUE;
}

/** Process PAUSE request */
static apt_bool_t sippulse_synth_channel_pause(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	sippulse_synth_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = TRUE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process RESUME request */
static apt_bool_t sippulse_synth_channel_resume(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	sippulse_synth_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = FALSE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process SET-PARAMS request */
static apt_bool_t sippulse_synth_channel_set_params(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	mrcp_synth_header_t *req_synth_header;
	/* get synthesizer header */
	req_synth_header = mrcp_resource_header_get(request);
	if(req_synth_header) {
		/* check voice age header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_AGE) == TRUE) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set Voice Age [%"APR_SIZE_T_FMT"]",
				req_synth_header->voice_param.age);
		}
		/* check voice name header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set Voice Name [%s]",
				req_synth_header->voice_param.name.buf);
		}
	}
	
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process GET-PARAMS request */
static apt_bool_t sippulse_synth_channel_get_params(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	mrcp_synth_header_t *req_synth_header;
	/* get synthesizer header */
	req_synth_header = mrcp_resource_header_get(request);
	if(req_synth_header) {
		mrcp_synth_header_t *res_synth_header = mrcp_resource_header_prepare(response);
		/* check voice age header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_AGE) == TRUE) {
			res_synth_header->voice_param.age = 25;
			mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_VOICE_AGE);
		}
		/* check voice name header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			apt_string_set(&res_synth_header->voice_param.name,"David");
			mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_VOICE_NAME);
		}
	}

	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Dispatch MRCP request */
static apt_bool_t sippulse_synth_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case SYNTHESIZER_SET_PARAMS:
			processed = sippulse_synth_channel_set_params(channel,request,response);
			break;
		case SYNTHESIZER_GET_PARAMS:
			processed = sippulse_synth_channel_get_params(channel,request,response);
			break;
		case SYNTHESIZER_SPEAK:
			processed = sippulse_synth_channel_speak(channel,request,response);
			break;
		case SYNTHESIZER_STOP:
			processed = sippulse_synth_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_PAUSE:
			processed = sippulse_synth_channel_pause(channel,request,response);
			break;
		case SYNTHESIZER_RESUME:
			processed = sippulse_synth_channel_resume(channel,request,response);
			break;
		case SYNTHESIZER_BARGE_IN_OCCURRED:
			processed = sippulse_synth_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_CONTROL:
			break;
		case SYNTHESIZER_DEFINE_LEXICON:
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
	}
	return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t sippulse_synth_stream_destroy(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t sippulse_synth_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t sippulse_synth_stream_close(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to read/get new frame */
static apt_bool_t sippulse_synth_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame)
{
	sippulse_synth_channel_t *synth_channel = stream->obj;
	/* check if STOP was requested */
	if(synth_channel->stop_response) {
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(synth_channel->channel,synth_channel->stop_response);
		synth_channel->stop_response = NULL;
		synth_channel->speak_request = NULL;
		synth_channel->paused = FALSE;
		if(synth_channel->audio_file) {
			fclose(synth_channel->audio_file);
			synth_channel->audio_file = NULL;
		}
		return TRUE;
	}

	/* check if there is active SPEAK request and it isn't in paused state */
	if(synth_channel->speak_request && synth_channel->paused == FALSE) {
		/* normal processing */
		apt_bool_t completed = FALSE;
		if(synth_channel->audio_file) {
			/* read speech from file */
			apr_size_t size = frame->codec_frame.size;
			if(fread(frame->codec_frame.buffer,1,size,synth_channel->audio_file) == size) {
				frame->type |= MEDIA_FRAME_TYPE_AUDIO;
			}
			else {
				completed = TRUE;
			}
		}
		else {
			/* fill with silence in case no file available */
			if(synth_channel->time_to_complete >= stream->rx_descriptor->frame_duration) {
				memset(frame->codec_frame.buffer,0,frame->codec_frame.size);
				frame->type |= MEDIA_FRAME_TYPE_AUDIO;
				synth_channel->time_to_complete -= stream->rx_descriptor->frame_duration;
			}
			else {
				completed = TRUE;
			}
		}
		
		if(completed) {
			/* raise SPEAK-COMPLETE event */
			mrcp_message_t *message = mrcp_event_create(
								synth_channel->speak_request,
								SYNTHESIZER_SPEAK_COMPLETE,
								synth_channel->speak_request->pool);
			if(message) {
				/* get/allocate synthesizer header */
				mrcp_synth_header_t *synth_header = mrcp_resource_header_prepare(message);
				if(synth_header) {
					/* set completion cause */
					synth_header->completion_cause = SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
					mrcp_resource_header_property_add(message,SYNTHESIZER_HEADER_COMPLETION_CAUSE);
				}
				/* set request state */
				message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

				synth_channel->speak_request = NULL;
				if(synth_channel->audio_file) {
					fclose(synth_channel->audio_file);
					synth_channel->audio_file = NULL;
				}
				/* send asynch event */
				mrcp_engine_channel_message_send(synth_channel->channel,message);
			}
		}
	}
	return TRUE;
}

static apt_bool_t sippulse_synth_msg_signal(sippulse_synth_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t status = FALSE;
	sippulse_synth_channel_t *sippulse_channel = channel->method_obj;
	sippulse_synth_engine_t *sippulse_engine = sippulse_channel->sippulse_engine;
	apt_task_t *task = apt_consumer_task_base_get(sippulse_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		sippulse_synth_msg_t *sippulse_msg;
		msg->type = TASK_MSG_USER;
		sippulse_msg = (sippulse_synth_msg_t*) msg->data;

		sippulse_msg->type = type;
		sippulse_msg->channel = channel;
		sippulse_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	return status;
}

static apt_bool_t sippulse_synth_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	sippulse_synth_msg_t *sippulse_msg = (sippulse_synth_msg_t*)msg->data;
	switch(sippulse_msg->type) {
		case SIPPULSE_SYNTH_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
			mrcp_engine_channel_open_respond(sippulse_msg->channel,TRUE);
			break;
		case SIPPULSE_SYNTH_MSG_CLOSE_CHANNEL:
			/* close channel, make sure there is no activity and send asynch response */
			mrcp_engine_channel_close_respond(sippulse_msg->channel);
			break;
		case SIPPULSE_SYNTH_MSG_REQUEST_PROCESS:
			sippulse_synth_channel_request_dispatch(sippulse_msg->channel,sippulse_msg->request);
			break;
		default:
			break;
	}
	return TRUE;
}
