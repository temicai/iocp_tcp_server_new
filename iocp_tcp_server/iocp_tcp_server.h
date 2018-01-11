#ifndef IOCP_TCP_SERVER_H_5E64EEE213C140A6A367B660325BF0AA
#define IOCP_TCP_SERVER_H_5E64EEE213C140A6A367B660325BF0AA

#include "tcp_common.h"

#define MAX_PARALLEL_INSTANCE_NUM 64

extern "C"
{
	unsigned long long __stdcall TS_StartServer(
		unsigned short usPort,
		fMessageCallback fMsgCb,
		void * pUserData,
		int nIdleTime
	);
	int __stdcall TS_StopServer(
		unsigned long long ullInst
	);
	int __stdcall TS_SendData(
		unsigned long long ullInst,
		const char * pEndpoint,
		const char * pData,
		unsigned int uiDataLen
	);
	int __stdcall TS_SetMessageCallback(
		unsigned long long ullInst,
		fMessageCallback fMsgCb,
		void * pUserData
	);
	int __stdcall TS_CloseEndpoint(
		unsigned long long ullInst,
		const char * pEndpoint
	);
	int __stdcall TS_GetPort(
		unsigned long long ullInst,
		unsigned short & usPort
	);
	int __stdcall TS_SetLogType(
		unsigned long long ullInst,
		unsigned short usLogType
	);


}
#endif
