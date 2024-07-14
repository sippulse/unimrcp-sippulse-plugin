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

#include "mrcp_recog_engine.h"
#include "mpf_activity_detector.h"
#include "apt_consumer_task.h"
#include "apt_log.h"
#include "mrcp_generic_header.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <samplerate.h>
#include <unistd.h>
#include <sys/types.h>

#define RECOG_ENGINE_TASK_NAME "SipPulse Recog Engine"

/*declarado antes apenas por causa da sequencia*/
typedef struct sippulse_recog_engine_t sippulse_recog_engine_t;
typedef struct sippulse_recog_channel_t sippulse_recog_channel_t;
typedef struct sippulse_recog_msg_t sippulse_recog_msg_t;

/** GET the API from the enviroment variable SIPPULSE_API_KEY */
const char* get_api_key() {
    return getenv("SIPPULSE_API_KEY");
}

#define SIPPULSE_API_KEY get_api_key()

struct MemoryStruct {
  char *memory;
  size_t size;
};

typedef struct {
    long http_code; // HTTP status code
    char *response_body; // Buffer for the HTTP response body
    size_t response_size; // Size of the response body
} http_response_t;

/** Declaration dos métodos de reconhecimento do engine */
static apt_bool_t sippulse_recog_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t sippulse_recog_engine_open(mrcp_engine_t *engine);
static apt_bool_t sippulse_recog_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* sippulse_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);
static apt_bool_t sippulse_recog_recognition_complete(sippulse_recog_channel_t *recog_channel, mrcp_recog_completion_cause_e cause);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	sippulse_recog_engine_destroy,
	sippulse_recog_engine_open,
	sippulse_recog_engine_close,
	sippulse_recog_engine_channel_create
};

/** Declaration of recognizer channel methods */
static apt_bool_t sippulse_recog_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t sippulse_recog_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t sippulse_recog_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t sippulse_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	sippulse_recog_channel_destroy,
	sippulse_recog_channel_open,
	sippulse_recog_channel_close,
	sippulse_recog_channel_request_process
};

/** Declaration of recognizer audio stream methods */
static apt_bool_t sippulse_recog_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t sippulse_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t sippulse_recog_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t sippulse_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	sippulse_recog_stream_destroy,
	NULL,
	NULL,
	NULL,
	sippulse_recog_stream_open,
	sippulse_recog_stream_close,
	sippulse_recog_stream_write,
	NULL
};

/** Declaration of SipPulse recognizer engine */
struct sippulse_recog_engine_t {
	apt_consumer_task_t    *task;
};

/** Declaração do canal de reconhecimento do SipPulse */
struct sippulse_recog_channel_t {
	/** Back pointer to engine */
	sippulse_recog_engine_t *sippulse_engine;
	/** Engine channel base */
	mrcp_engine_channel_t   *channel;
	/** Active (in-progress) recognition request */
	mrcp_message_t          *recog_request;
	/** Pending stop response */
	mrcp_message_t          *stop_response;
	/** Indicates whether input timers are started */
	apt_bool_t               timers_started;
	/** Voice activity detector */
	mpf_activity_detector_t *detector;
	/** File to write utterance to */
	FILE                    *audio_out;
	/** File to write wav resampled */
	http_response_t 		*http_response;
	//** Prompt to pass to the API */
	char  		            *prompt;
	//** Language to pass to the API */
	apt_str_t               *language;
	//** Model to pass to the API */
	apt_str_t               *model;
};

typedef enum {
	sippulse_recog_MSG_OPEN_CHANNEL,
	sippulse_recog_MSG_CLOSE_CHANNEL,
	sippulse_recog_MSG_REQUEST_PROCESS
} sippulse_recog_msg_type_e;

/** Declaration of SipPulse recognizer task message */
struct sippulse_recog_msg_t {
	sippulse_recog_msg_type_e  type;
	mrcp_engine_channel_t *channel; 
	mrcp_message_t        *request;
};

static apt_bool_t sippulse_recog_msg_signal(sippulse_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t sippulse_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="RECOG-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(RECOG_PLUGIN,"RECOG-PLUGIN")

/** Use custom log source mark */
#define RECOG_LOG_MARK   APT_LOG_MARK_DECLARE(RECOG_PLUGIN)

/** Executado no momento da carga do plugin */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	sippulse_recog_engine_t *sippulse_engine = (sippulse_recog_engine_t*)apr_palloc(pool,sizeof(sippulse_recog_engine_t));
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(sippulse_recog_msg_t),pool);
	sippulse_engine->task = apt_consumer_task_create(sippulse_engine,msg_pool,pool);
	if(!sippulse_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(sippulse_engine->task);
	apt_task_name_set(task,RECOG_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = sippulse_recog_msg_process;
	}

	/* create engine base */
	return mrcp_engine_create(
				MRCP_RECOGNIZER_RESOURCE,  /* MRCP resource identifier */
				sippulse_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

// Function to find the value of the parameter com.sippulse.prompt
const char* get_prompt_value(const apt_pair_arr_t *param_pairs, apr_pool_t *pool) {
    if (!param_pairs) {
        return NULL;
    }

    for (int i = 0; i < param_pairs->nelts; i++) {
        apt_pair_t *pair = &APR_ARRAY_IDX(param_pairs, i, apt_pair_t);
        if (pair && pair->name.buf && strcasecmp(pair->name.buf, "com.sippulse.prompt") == 0) {
            apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Prompt for whisper: %s", pair->value.buf);
			return pair->value.buf;
        }
    }
    return NULL;
}


// Função para criar a resposta NLSML
char* createNLSMLResponse(const char *result) {
    // Estimate the size needed for the NLSML string, adjusting as necessary
    size_t baseSize = strlen(result) + strlen(result) + 512; // Buffer size to account for fixed parts and dynamic content

    char* nlsml = (char*)malloc(baseSize * sizeof(char));
    if (nlsml == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        return NULL;
    }

    snprintf(nlsml, baseSize, "<?xml version=\"1.0\"?>\n"
                              "<result>\n"
                              "	<interpretation grammar=\"builtin:speech/transcribe\" confidence=\"1.00\">\n"
                              "		<instance>%s</instance>\n"
                              "		<input mode=\"speech\">%s</input>\n"
                              "	</interpretation>\n"
                              "</result>\n", result, result);

    return nlsml;
}

/* CURL Write Call Back Function */
size_t my_curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;
 
  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if(!ptr) {
    /* out of memory! */
    printf("not enough memory (realloc returned NULL)\n");
    return 0;
  }
 
  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
  return realsize;

}

/**
 * Performs an HTTP POST request to the Sippulse.ai API for speech recognition.
 * 
 * @param audio_data The audio data buffer.
 * @param audio_size The size of the audio data buffer.
 * @return 0 on success, non-zero on error.
 */
int sippulse_recognizer_accept_waveform(sippulse_recog_channel_t *recog_channel) {
	CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    struct curl_httppost *formpost = NULL;
    struct curl_httppost *lastptr = NULL;
    long http_code = 0;
	long frame_size=0;
	char * frame_buffer = NULL;
	struct MemoryStruct chunk;
  	chunk.memory = malloc(1);  /* grown as needed by the realloc above */
  	chunk.size = 0;    /* no data at this point */


	 // Seek to the end of the file to determine the size
    fseek(recog_channel->audio_out, 0, SEEK_END);
    frame_size = ftell(recog_channel->audio_out);
    rewind(recog_channel->audio_out); // Go back to the start of the file

    // Allocate memory for the entire file content plus a null terminator
    frame_buffer = (char*)malloc(frame_size + 1);
    if (frame_buffer == NULL) {
        perror("Memory error");
		free(frame_buffer);
        return 0;
    }

	// Read the file into the buffer
    size_t bytesRead = fread(frame_buffer, 1, frame_size, recog_channel->audio_out);
	
	if (bytesRead != frame_size) {
        perror("Reading error");
        free(frame_buffer);
        return 0;
    }

	// Initialize the chunk memory
	chunk.memory = malloc(1);
	chunk.size = 0;

	// Initialize CURL
	curl_global_init(CURL_GLOBAL_ALL);
	curl = curl_easy_init();
	if (curl) {
		char url[8192];
		
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Language for whisper: %s", recog_channel->language->buf);
		

		//Set defaults
		//if(recog_channel->model->buf=="" || recog_channel->model->buf==NULL) {
		//	recog_channel->model->buf="whisper-1";
		//}

		if(recog_channel->prompt=="" || recog_channel->prompt==NULL) {
			snprintf(url, sizeof(url), "https://api.sippulse.ai/v1/asr/transcribe?language=%s&response_format=text&model=%s", recog_channel->language->buf, recog_channel->model->buf);
		} else {
			snprintf(url, sizeof(url), "https://api.sippulse.ai/v1/asr/transcribe?language=%s&response_format=text&model=%s&prompt=%s", recog_channel->language->buf, recog_channel->model->buf,recog_channel->prompt);
		}

		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"URL: %s", url);
		curl_easy_setopt(curl, CURLOPT_URL, url);
	
		// Prepare headers
		headers = curl_slist_append(headers, "accept: application/json");
		char auth_header[256];
		snprintf(auth_header, sizeof(auth_header), "api-key: %s", SIPPULSE_API_KEY);
		headers = curl_slist_append(headers, auth_header);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

		// The rest of the function setup remains the same until CURLOPT_HTTPPOST setting
		// Here, use CURLFORM_BUFFERPTR instead of CURLFORM_FILE
		curl_formadd(&formpost, &lastptr,
             CURLFORM_COPYNAME, "file",
             CURLFORM_BUFFER, "speech1.pcm",
             CURLFORM_BUFFERPTR, frame_buffer,
             CURLFORM_BUFFERLENGTH, frame_size,
             CURLFORM_CONTENTTYPE, "audio/pcm;rate=8000;encoding=signed-int;channels=1;big-endian=false",
             CURLFORM_END);

		curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);

		// Set the write callback function
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, my_curl_write_callback);

		// Pass the recog_channel as the userp argument to the callback function
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

		// Perform the request
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Performing request");

		res = curl_easy_perform(curl);
	
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Request performed");

	
		if (res != CURLE_OK) {
			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
			curl_easy_cleanup(curl);
			curl_formfree(formpost);
			curl_slist_free_all(headers);
			curl_global_cleanup();
			free(frame_buffer);
			free(chunk.memory);
			//fclose(recog_channel->audio_out);
			return 0; // Indicate error
		}

        // Check HTTP response code
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code == 200) {
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Server returned HTTP %ld", http_code);
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Response: %s|", chunk.memory);
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Response size: %d|", chunk.size);
			// Success, now prepare to trigger recognition complete event
    		// This is where you'd typically analyze the response to set the appropriate cause
			recog_channel->http_response = malloc(sizeof(http_response_t));
			recog_channel->http_response->http_code = http_code;
			recog_channel->http_response->response_body = (char *)malloc(chunk.size);
    		if (recog_channel->http_response->response_body == NULL) {
        		// Handle malloc failure
        		free(recog_channel->http_response);
        		return 0;
    		}

			// Copy the response body
    		memcpy(recog_channel->http_response->response_body, chunk.memory, chunk.size);

   			// Null-terminate the response body, replace the \n with \0
    		recog_channel->http_response->response_body[chunk.size-1] = '\0';

    		// Set the correct response size (excluding the null terminator for the size)
    		recog_channel->http_response->response_size = chunk.size;
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Response: saved %s|", recog_channel->http_response->response_body);
        } else {
			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Server returned HTTP %ld\n", http_code);
            curl_easy_cleanup(curl);
            curl_formfree(formpost);
            curl_slist_free_all(headers);
            curl_global_cleanup();
			free(frame_buffer);
			free(chunk.memory);
			//fclose(recog_channel->audio_out);
			return 0; // Indicate error
		}

        // Cleanup
        curl_easy_cleanup(curl);
        curl_formfree(formpost);
        curl_slist_free_all(headers);
        curl_global_cleanup();
		free(frame_buffer);
		free(chunk.memory);
		if (ftruncate(fileno(recog_channel->audio_out), 0) != 0) {
				apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to truncate file");
		}
		//fclose(recog_channel->audio_out);
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Everything clean");

    } else {
        fprintf(stderr, "Could not initialize curl.\n");
        return 0; // Indicate error
    }
    return 1; // Success
}

/** Destroy recognizer engine */
static apt_bool_t sippulse_recog_engine_destroy(mrcp_engine_t *engine)
{
	sippulse_recog_engine_t *sippulse_engine = (sippulse_recog_engine_t*)engine->obj;
	if(sippulse_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(sippulse_engine->task);
		apt_task_destroy(task);
		sippulse_engine->task = NULL;
	}
	return TRUE;
}

/** Open recognizer engine */
static apt_bool_t sippulse_recog_engine_open(mrcp_engine_t *engine)
{
	sippulse_recog_engine_t *sippulse_engine = (sippulse_recog_engine_t*)engine->obj;
	if(sippulse_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(sippulse_engine->task);
		apt_task_start(task);
	}
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close recognizer engine */
static apt_bool_t sippulse_recog_engine_close(mrcp_engine_t *engine)
{
	sippulse_recog_engine_t *sippulse_engine = (sippulse_recog_engine_t*)engine->obj;
	if(sippulse_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(sippulse_engine->task);
		apt_task_terminate(task,TRUE);
	}
	return mrcp_engine_close_respond(engine);
}

static mrcp_engine_channel_t* sippulse_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination; 

	/* create sippulse recog channel */
	sippulse_recog_channel_t *recog_channel = (sippulse_recog_channel_t*)apr_palloc(pool,sizeof(sippulse_recog_channel_t));
	recog_channel->sippulse_engine = (sippulse_recog_engine_t*)engine->obj;
	recog_channel->recog_request = NULL;
	recog_channel->stop_response = NULL;
	recog_channel->detector = mpf_activity_detector_create(pool);
	recog_channel->audio_out = NULL;

	capabilities = mpf_sink_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
			"LPCM");

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			recog_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	recog_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			recog_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	return recog_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t sippulse_recog_channel_destroy(mrcp_engine_channel_t *channel)
{
	/* nothing to destrtoy */
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t sippulse_recog_channel_open(mrcp_engine_channel_t *channel)
{
	return sippulse_recog_msg_signal(sippulse_recog_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t sippulse_recog_channel_close(mrcp_engine_channel_t *channel)
{
	return sippulse_recog_msg_signal(sippulse_recog_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t sippulse_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	return sippulse_recog_msg_signal(sippulse_recog_MSG_REQUEST_PROCESS,channel,request);
}

/** Process RECOGNIZE request */
static apt_bool_t sippulse_recog_channel_recognize(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process RECOGNIZE request */
	mrcp_recog_header_t *recog_header;
	mrcp_generic_header_t *generic_header;
	apt_str_t *params_pair;
	apt_pair_arr_t *pair_array;
	apr_pool_t *pool;
	mrcp_vendor_specific_params_list_t *vendor_specific_params_list;
	sippulse_recog_channel_t *recog_channel = (sippulse_recog_channel_t*)channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_sink_stream_codec_get(channel);
	
	if(!descriptor) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		return FALSE;
	}

	recog_channel->timers_started = TRUE;

	/* get recognizer header */
	recog_header = (mrcp_recog_header_t*)mrcp_resource_header_get(request);
	if(recog_header) {
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_START_INPUT_TIMERS) == TRUE) {
			recog_channel->timers_started = recog_header->start_input_timers;
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_NO_INPUT_TIMEOUT) == TRUE) {
			mpf_activity_detector_noinput_timeout_set(recog_channel->detector,recog_header->no_input_timeout);
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT) == TRUE) {
			mpf_activity_detector_silence_timeout_set(recog_channel->detector,recog_header->speech_complete_timeout);
		}

		//Get the other recog headers
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_SPEECH_LANGUAGE) == TRUE) {
			recog_channel->language = &recog_header->speech_language;
		}

		//Get the other recog headers
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_RECOGNITION_MODE) == TRUE) {
			recog_channel->model = &recog_header->recognition_mode;
		}

		//Get the other recog headers
		if(mrcp_generic_header_property_check(request,GENERIC_HEADER_VENDOR_SPECIFIC_PARAMS) == TRUE) {
			mrcp_generic_header_t *generic_header = mrcp_generic_header_get(request);
			recog_channel->prompt=NULL;
			pair_array = generic_header->vendor_specific_params;
			for (int i = 0; i < pair_array->nelts; i++) {
        		apt_pair_t *pair = &APR_ARRAY_IDX(pair_array, i, apt_pair_t);
        		if (pair && pair->name.buf && strcasecmp(pair->name.buf, "com.sippulse.prompt") == 0) {
            		recog_channel->prompt=pair->value.buf;
				}
			}
			//recog_channel->prompt=prompt_value;
			//const char *prompt_value=get_prompt_value(pair_array,pool);
			//apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Prompt for whisper: %s", recog_channel->prompt->buf);
		}
	}

	if(!recog_channel->audio_out) {
		const apt_dir_layout_t *dir_layout = channel->engine->dir_layout;
		char *file_name = apr_psprintf(channel->pool,"utter-%dkHz-%s.pcm",
							descriptor->sampling_rate/1000,
							request->channel_id.session_id.buf);
		char *file_path = apt_vardir_filepath_get(dir_layout,file_name,channel->pool);
		if(file_path) {
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Open Utterance Output File [%s] for Writing",file_path);
			recog_channel->audio_out = fopen(file_path,"wb+");
			if(!recog_channel->audio_out) {
				apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Open Utterance Output File [%s] for Writing",file_path);
			} 
		} else {
			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Generate Utterance Output File Path");	
		}
	}

	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	recog_channel->recog_request = request;
	return TRUE;
}

/** Process STOP request */
static apt_bool_t sippulse_recog_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process STOP request */
	sippulse_recog_channel_t *recog_channel = (sippulse_recog_channel_t*)channel->method_obj;
	/* store STOP request, make sure there is no more activity and only then send the response */
	recog_channel->stop_response = response;
	return TRUE;
}

/** Process START-INPUT-TIMERS request */
static apt_bool_t sippulse_recog_channel_timers_start(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	sippulse_recog_channel_t *recog_channel = (sippulse_recog_channel_t*)channel->method_obj;
	recog_channel->timers_started = TRUE;
	return mrcp_engine_channel_message_send(channel,response);
}

/** Dispatch MRCP request */
static apt_bool_t sippulse_recog_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case RECOGNIZER_SET_PARAMS:
			break;
		case RECOGNIZER_GET_PARAMS:
			break;
		case RECOGNIZER_DEFINE_GRAMMAR:
			break;
		case RECOGNIZER_RECOGNIZE:
			processed = sippulse_recog_channel_recognize(channel,request,response);
			break;
		case RECOGNIZER_GET_RESULT:
			break;
		case RECOGNIZER_START_INPUT_TIMERS:
			processed = sippulse_recog_channel_timers_start(channel,request,response);
			break;
		case RECOGNIZER_STOP:
			processed = sippulse_recog_channel_stop(channel,request,response);
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
static apt_bool_t sippulse_recog_stream_destroy(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t sippulse_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t sippulse_recog_stream_close(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/* Raise sippulse START-OF-INPUT event */
static apt_bool_t sippulse_recog_start_of_input(sippulse_recog_channel_t *recog_channel)
{
	/* create START-OF-INPUT event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_START_OF_INPUT,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}

	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

/* Raise sippulse RECOGNITION-COMPLETE event */
static apt_bool_t sippulse_recog_recognition_complete(sippulse_recog_channel_t *recog_channel, mrcp_recog_completion_cause_e cause)
{
	
	mrcp_recog_header_t *recog_header;

	/* create RECOGNITION-COMPLETE event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_RECOGNITION_COMPLETE,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}

	/* get allocate recognizer header */
	recog_header = (mrcp_recog_header_t*)mrcp_resource_header_prepare(message);
	if(recog_header) {
		/* set completion cause */
		recog_header->completion_cause = cause;
		mrcp_resource_header_property_add(message,RECOGNIZER_HEADER_COMPLETION_CAUSE);
	}

	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

	if(cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS) {
			/* load recognition result */
			const char *result = recog_channel->http_response->response_body;
			const int result_size = recog_channel->http_response->response_size;
			
			char* nlsmlResult = createNLSMLResponse(result);
			
    		if (nlsmlResult != NULL) {
        		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"NLSML Response created: %s", nlsmlResult);
				//Send plain text instead
				//apt_string_assign_n(&message->body,result,strlen(result),message->pool);
				apt_string_assign_n(&message->body,nlsmlResult,strlen(nlsmlResult),message->pool);
				free(nlsmlResult); // Remember to free the allocated memory
    		} else {
				free(nlsmlResult); // Remember to free the allocated memory
				apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to create NLSML Response");
				return FALSE;
			}
				
			/* get/allocate generic header */
			mrcp_generic_header_t *generic_header = mrcp_generic_header_prepare(message);
			if(generic_header) {
				/* set content types */
				//apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Setting Content Type to text/plain");
				//apt_string_assign(&generic_header->content_type,"text/plain; charset=UTF-8",message->pool);
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Setting Content Type to application/nlsml");
				apt_string_assign(&generic_header->content_type,"application/nlsml",message->pool);
				mrcp_generic_header_property_add(message,GENERIC_HEADER_CONTENT_TYPE);
			}
	}

	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Before Recog Channel " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));

	recog_channel->recog_request = NULL;

	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

/** Callback is called from MPF engine context to write/send new frame */
static apt_bool_t sippulse_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
	sippulse_recog_channel_t *recog_channel = (sippulse_recog_channel_t*)stream->obj;
	if(recog_channel->stop_response) {
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(recog_channel->channel,recog_channel->stop_response);
		recog_channel->stop_response = NULL;
		recog_channel->recog_request = NULL;
		return TRUE;
	}

	if(recog_channel->recog_request) {
		mpf_detector_event_e det_event = mpf_activity_detector_process(recog_channel->detector,frame);
		switch(det_event) {
			case MPF_DETECTOR_EVENT_ACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Activity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				sippulse_recog_start_of_input(recog_channel);
				break;
			case MPF_DETECTOR_EVENT_INACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Inactivity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				
				if(!sippulse_recognizer_accept_waveform(recog_channel)) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Accept Waveform " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				} else {
					sippulse_recog_recognition_complete(recog_channel, RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
				}
				break;
			case MPF_DETECTOR_EVENT_NOINPUT:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Noinput " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				if(recog_channel->timers_started == TRUE) {
					sippulse_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT);
				}
				break;
			default:
				break;
		}

		if(recog_channel->recog_request) {
			if((frame->type & MEDIA_FRAME_TYPE_EVENT) == MEDIA_FRAME_TYPE_EVENT) {
				if(frame->marker == MPF_MARKER_START_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Start of Event " APT_SIDRES_FMT " id:%d",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id);
				}
				else if(frame->marker == MPF_MARKER_END_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected End of Event " APT_SIDRES_FMT " id:%d duration:%d ts",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id,
						frame->event_frame.duration);
				}
			}
		}

		if(recog_channel->audio_out) {
			fwrite(frame->codec_frame.buffer,1,frame->codec_frame.size,recog_channel->audio_out);
		}

	}
	return TRUE;
}

static apt_bool_t sippulse_recog_msg_signal(sippulse_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t status = FALSE;
	sippulse_recog_channel_t *sippulse_channel = (sippulse_recog_channel_t*)channel->method_obj;
	sippulse_recog_engine_t *sippulse_engine = sippulse_channel->sippulse_engine;
	apt_task_t *task = apt_consumer_task_base_get(sippulse_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		sippulse_recog_msg_t *sippulse_msg;
		msg->type = TASK_MSG_USER;
		sippulse_msg = (sippulse_recog_msg_t*) msg->data;

		sippulse_msg->type = type;
		sippulse_msg->channel = channel;
		sippulse_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	return status;
}

static apt_bool_t sippulse_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	sippulse_recog_msg_t *sippulse_msg = (sippulse_recog_msg_t*)msg->data;
	switch(sippulse_msg->type) {
		case sippulse_recog_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
			mrcp_engine_channel_open_respond(sippulse_msg->channel,TRUE);
			break;
		case sippulse_recog_MSG_CLOSE_CHANNEL:
		{
			/* close channel, make sure there is no activity and send asynch response */
			sippulse_recog_channel_t *recog_channel = (sippulse_recog_channel_t*)sippulse_msg->channel->method_obj;

			if(NULL == recog_channel->audio_out) {
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"File already closed ");
			} else {
				fclose(recog_channel->audio_out); 
				recog_channel->audio_out = NULL;
			}
			
			mrcp_engine_channel_close_respond(sippulse_msg->channel);

			break;
		}
		case sippulse_recog_MSG_REQUEST_PROCESS:
			sippulse_recog_channel_request_dispatch(sippulse_msg->channel,sippulse_msg->request);
			break;
		default:
			break;
	}
	return TRUE;
}