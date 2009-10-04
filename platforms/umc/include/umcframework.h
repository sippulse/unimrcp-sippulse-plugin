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

#ifndef __UMC_FRAMEWORK_H__
#define __UMC_FRAMEWORK_H__

/**
 * @file umcframework.h
 * @brief UMC Application Framework
 */ 

#include <apr_hash.h>
#include "mrcp_application.h"
#include "apt_consumer_task.h"

class UmcSession;

class UmcFramework
{
public:
/* ============================ CREATORS =================================== */
	UmcFramework();
	~UmcFramework();

/* ============================ MANIPULATORS =============================== */
	bool Create(apt_dir_layout_t* pDirLayout, apr_pool_t* pool);
	void Destroy();

	int RunSession(const char* pScenarioName, const char* pProfileName);
	void KillSession(int id);

protected:
	bool CreateMrcpClient();
	void DestroyMrcpClient();

	bool CreateTask();
	void DestroyTask();

	bool LoadScenarios();
	void DestroyScenarios();

	bool ProcessRunRequest(int id, const char* pScenarioName, const char* pProfileName);
	void ProcessKillRequest(int id);

	bool AddSession(UmcSession* pSession);
	bool RemoveSession(UmcSession* pSession);

/* ============================ HANDLERS =================================== */
	friend apt_bool_t UmcProcessMsg(apt_task_t* pTask, apt_task_msg_t* pMsg);
	friend void UmcOnStartComplete(apt_task_t* pTask);
	friend void UmcOnTerminateComplete(apt_task_t* pTask);

	friend apt_bool_t AppMessageHandler(const mrcp_app_message_t* pAppMessage);
	friend apt_bool_t OnReady(mrcp_application_t* pMrcpApplication, mrcp_sig_status_code_e status);
	friend apt_bool_t OnSessionTerminate(mrcp_application_t *application, mrcp_session_t *session, mrcp_sig_status_code_e status);

private:
/* ============================ DATA ======================================= */
	apr_pool_t*          m_pPool;
	apt_dir_layout_t*    m_pDirLayout;
	apt_consumer_task_t* m_pTask;

	mrcp_client_t*       m_pMrcpClient;
	mrcp_application_t*  m_pMrcpApplication;

	int                  m_CurSessionId;
	bool                 m_Ready;

	apr_hash_t*          m_pScenarioTable;
	apr_hash_t*          m_pSessionTable;
};

#endif /*__UMC_FRAMEWORK_H__*/