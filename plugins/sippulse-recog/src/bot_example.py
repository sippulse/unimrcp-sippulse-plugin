#!/usr/bin/python3
# -*- coding: utf-8 -*-
"""
    AGI for Agent conversation
    This agi script is to be used with SipPulse AI
    
    * Revision: 0.1
    * Date: July 5, 2024
    * Vendor: SipPulse Tecnologia Ltda.
"""

import sys
from asterisk.agi import *
import requests
import os
import json
import re
import urllib
import time
from apscheduler.schedulers.background import BackgroundScheduler
from datetime import datetime, timedelta
import sys

SIPPULSE_API_KEY = os.getenv('SIPPULSE_API_KEY', 'sp-3236c540366143c3b77504e79fcde0ef')
THREAD_API_URL = os.getenv('THREAD_API_URL', 'https://api.dev.sippulse.ai/v1/threads')
AGENT_ID = os.getenv('AGENT_ID','agt_970178a6ef3749e28969d')
AGENT_INACTIVITY_THRESHOLD = os.getenv('INACTIVITY', 2)
PROMPT = 'Obrigado por ligar para SipPulse Airlines, me diga o número do vôo e a data de partida'
GRAMMAR = "builtin:speech/transcribe"
VOICE="pt-BR-FranciscaNeural"
SPL="pt"
BARGE_IN='0'
RECOGNITION_TIMEOUT='5000'
MODEL="whisper-1"
WHISPER_PROMPT='com.sippulse.prompt=""'
threads_activity = {}

agi = AGI()
agi.verbose('Start running')

class SipPulseBotApp:

    """A class representing SipPulse bot application"""

    def __init__(self, options):
        """Constructor"""
        self.options = options
        self.recog_options = recog_options
        self.status = None
        self.cause = None

    def synth_and_recog(self):
        args = "\\\"%s\\\",%s,%s" % (self.prompt,GRAMMAR,self.options)
        agi.set_variable('RECOG_STATUS', '')
        agi.set_variable('RECOG_COMPLETION_CAUSE', '')
        self.action = None
        agi.appexec('SynthandRecog', args)
        self.status = agi.get_variable('RECOG_STATUS')
        agi.verbose('got status %s' % self.status)
        if self.status == 'OK':
            self.cause = agi.get_variable('RECOG_COMPLETION_CAUSE')
            agi.verbose('got completion cause %s' % self.cause)
        else:
            agi.verbose('Recognition completed abnormally')

    def recog(self):
        agi.verbose('Start recog')
        args = "\\\"%s\\\",%s" % (
            GRAMMAR, self.recog_options)
        agi.set_variable('RECOGSTATUS', '')
        agi.set_variable('RECOG_COMPLETION_CAUSE', '')
        self.action = None
        agi.verbose('Executing MRCPRecog')  
        agi.appexec('MRCPRecog', args)
        self.status = agi.get_variable('RECOGSTATUS')
        agi.verbose('got status %s' % self.status)
        if self.status == 'OK':
            self.cause = agi.get_variable('RECOG_COMPLETION_CAUSE')
            agi.verbose('got completion cause %s' % self.cause)
        else:
            agi.verbose('Recognition completed abnormally')
        return 1

    def get_speak(self):
        """Retrieves message text from the data returned by bot"""
        speak=""
        caller_id = agi.get_variable('CALLERID(num)')
        agi.verbose('got caller_id %s' % caller_id)
        speak = agi.get_variable('RECOG_INSTANCE(0/0)')
        speak = speak.strip()
        agi.verbose('got speak %s' % speak)
        return speak

    def synth(self):
        """Synthesizes the message text"""
        agi.verbose('Synthesizing %s' % self.ai_message)
        agi.appexec('MRCPSynth', "\\\"%s\\\",v=pt-BR-FranciscaNeural" % self.ai_message)
        time.sleep(1)
        return 1
    
    def run(self):
        agi.verbose('Start running')
        processing = True
        self.prompt = PROMPT
        self.synth_and_recog()
        first_time = True
        while processing:
            processing = True
            if first_time:
                first_time = False
            else:
                #Execute second time
                agi.verbose('Executing second time')
                self.recog()
            if self.status == 'OK':
                #Cause 000 = Succcess in recognition
                if self.cause == '000':
                    self.prompt = self.get_speak()
                    agi.verbose('got prompt %s' % self.prompt)
            
                #Cause 001 = No Match or 002 = NoInput
                elif self.cause == '001' or self.cause == '002':
                    processing = False
                    break
                    #end of processing

            elif self.cause == '001' or self.cause == '002':
                processing = False
                break
                #End of processing
            
            self.ai_message=self.handle_recog(self.prompt)
            self.synth()
            agi.verbose('Synth executed')
            self.prompt=None
            
        self.prompt = 'Obrigado por ligar para SipPulse Airlines'
        agi.appexec('MRCPSynth', "\\\"%s\\\",v=pt-BR-FranciscaNeural" % self.prompt)

    def handle_recog(self,message):
        """Handles the recognition process"""
        #check if a thread with this sender id exists
        #agi.verbose('message=%s' % message)
        
        sender_id = agi.get_variable('CALLERID(num)')

        for thread_id, (last_activity, sender) in threads_activity.items():
            if sender == sender_id:
                threads_activity[thread_id] = (datetime.now(), sender_id)
                ai_message = self.run_thread(thread_id, "user", message)
                return ai_message
            
        # If no thread exists for the sender, create a new thread
        agi.verbose('Creating new thread')
        thread_id = self.create_thread()
        agi.verbose('Thread ID: %s' % thread_id)

        # Update the last activity time and sender ID for the thread
        threads_activity[thread_id] = (datetime.now(), sender_id)

        # Run the thread
        ai_message = self.run_thread(thread_id, "user", message)
        #agi.verbose('AI response:%s' % ai_message)
        return ai_message

    def create_thread(self):
        agi.verbose('Creating thread')
        url = THREAD_API_URL
        headers = {
            'Content-Type': 'application/json',
            'api-key': SIPPULSE_API_KEY
        }
        payload = {
            "agent_id": AGENT_ID
        }
       
        response = requests.post(url, headers=headers, json=payload)
        agi.verbose('Thread creation response: %s' % response.json())
        thread_id = response.json().get('id')
        agi.verbose('Thread created with ID: %s' % thread_id)
        return thread_id

    def run_thread(self,thread_id, role, content):
        url = f"{THREAD_API_URL}/{thread_id}/run"
        headers = {
            'Content-Type': 'application/json',
            'api-key': SIPPULSE_API_KEY
        }
        payload = {
            "role": role,
            "content": content
        }
        response = requests.post(url, headers=headers, json=payload)
        ai_message = response.json()['choices'][0]['message']['content']
        ai_message = re.sub(r'<result>|</result>', '', ai_message)
        #agi.verbose('Thread run response:%s' %ai_message)

        # Update the last activity time for the thread
        threads_activity[thread_id] = (datetime.now(), threads_activity[thread_id][1])
        return ai_message

    def clear_thread(self,thread_id):
        url = f"{THREAD_API_URL}/{thread_id}/clear"
        headers = {
            'Content-Type': 'application/json',
            'api-key': SIPPULSE_API_KEY
        }
        response = requests.post(url, headers=headers)
        agi.verbose('Thread %s cleared'% thread_id)

    def delete_thread(self,thread_id):
        url = f"{THREAD_API_URL}/{thread_id}"
        headers = {
            'Content-Type': 'application/json',
            'api-key': SIPPULSE_API_KEY
        }
        response = requests.delete(url, headers=headers)
        agi.vebose('Thread %s deleted'% thread_id)
        
        # Remove the thread from the activity tracking dictionary
        if thread_id in threads_activity:
            del threads_activity[thread_id]

    def check_inactive_threads(self):
        current_time = datetime.now()
        inactive_threshold = timedelta(minutes=AGENT_INACTIVITY_THRESHOLD)  # Define your inactivity threshold
        for thread_id, (last_activity,sender_id) in list(threads_activity.items()):
            if current_time - last_activity > inactive_threshold:
                self.synth("Esta conversa está sendo encerrada por inatividade, se quiser chame novamente e teremos o prazer de atendê-lo.")
                clear_thread(thread_id)
                delete_thread(thread_id)


agi.verbose('Start on the end of the script')

#Concatenating the options
options = 't=' + RECOGNITION_TIMEOUT + '&vn=' + VOICE + '&spl=' + SPL + '&b=' + BARGE_IN + '&rm=' + MODEL + '&ct=0.7' + '&vsp=' + WHISPER_PROMPT + "&uer=1"
recog_options = 't=' + RECOGNITION_TIMEOUT +'&spl=' + SPL + '&rm=' + MODEL + '&ct=0.7' + '&vsp=' + WHISPER_PROMPT + "&uer=1" + '&sl=0.75'

agi.verbose('options=',options)

botApp = SipPulseBotApp(options)

botApp.run()
agi.verbose('exiting')