#include "iocp_tcp_server.h"
#include "tcp_server_concrete.h"
#include <Windows.h>

typedef std::map<unsigned long long, ts_new::IocpTcpServer *> IocpTcpServerList;

static char g_szDllDir[256] = { 0 };
IocpTcpServerList g_serverList;
std::mutex g_mutex4ServerList;

BOOL APIENTRY DllMain(HMODULE hInst, DWORD dwReason, PVOID pReserved)
{
	switch (dwReason) {
		case DLL_PROCESS_ATTACH: {
			memset(g_szDllDir, 0, sizeof(g_szDllDir));
			char szPath[256] = { 0 };
			if (GetModuleFileNameA(hInst, szPath, sizeof(szPath)) != 0) {
				char szDrive[32] = { 0 };
				char szDir[256] = { 0 };
				_splitpath_s(szPath, szDrive, 32, szDir, 256, NULL, 0, NULL, 0);
				sprintf_s(g_szDllDir, sizeof(g_szDllDir), "%s%s", szDrive, szDir);
			}
			break;
		}
		case DLL_PROCESS_DETACH: {
			std::lock_guard<std::mutex> lock(g_mutex4ServerList);
			if (!g_serverList.empty()) {
				IocpTcpServerList::iterator iter = g_serverList.begin();
				while (iter != g_serverList.end()) {
					ts_new::IocpTcpServer * pServer = iter->second;
					if (pServer) {
						delete pServer;
						pServer = NULL;
					}
					iter = g_serverList.erase(iter);
				}
			}
			break;
		}
	}
	return TRUE;
}

unsigned long long __stdcall TS_StartServer(unsigned short usPort_, fMessageCallback fMsgCb_,
	void * pUserData_, int nIdleTime_)
{
	unsigned long long result = 0;
	std::lock_guard<std::mutex> lock(g_mutex4ServerList);
	if (g_serverList.size() < MAX_PARALLEL_INSTANCE_NUM) {
		ts_new::IocpTcpServer * pServer = new ts_new::IocpTcpServer(g_szDllDir);
		if (pServer) {
			if (pServer->Start(usPort_, (unsigned int)nIdleTime_) == 0) {
				pServer->SetMessageCallback(fMsgCb_, pUserData_);
				unsigned long long ullInst = (unsigned long long)pServer;
				g_serverList.emplace(ullInst, pServer);
				result = ullInst;
			}
			else {
				delete pServer;
				pServer = NULL;
			}
		}
	}
	return result;
}

int __stdcall TS_StopServer(unsigned long long ullInst_)
{
	int result = -1;
	std::lock_guard<std::mutex> lock(g_mutex4ServerList);
	if (!g_serverList.empty()) {
		IocpTcpServerList::iterator iter = g_serverList.find(ullInst_);
		if (iter != g_serverList.end()) {
			ts_new::IocpTcpServer * pServer = iter->second;
			if (pServer) {
				result = pServer->Stop();
				delete pServer;
				pServer = nullptr;
			}
			g_serverList.erase(iter);
		}
	}
	return result;
}

int __stdcall TS_SendData(unsigned long long ullInst_, const char * pEndpoint_, const char * pData_,
	unsigned int uiDataLen_)
{
	int result = -1;
	std::lock_guard<std::mutex> lock(g_mutex4ServerList);
	if (!g_serverList.empty()) {
		IocpTcpServerList::iterator iter = g_serverList.find(ullInst_);
		if (iter != g_serverList.end()) {
			ts_new::IocpTcpServer * pServer = iter->second;
			if (pServer) {
				result = pServer->SendData(pEndpoint_, pData_, uiDataLen_);
			}
		}
	}
	return result;
}

int __stdcall TS_SetMessageCallback(unsigned long long ullInst_, fMessageCallback fMsgCb_, void * pUserData_)
{
	int result = -1;
	std::lock_guard<std::mutex> lock(g_mutex4ServerList);
	if (!g_serverList.empty()) {
		IocpTcpServerList::iterator iter = g_serverList.find(ullInst_);
		if (iter != g_serverList.end()) {
			ts_new::IocpTcpServer * pServer = iter->second;
			if (pServer) {
				pServer->SetMessageCallback(fMsgCb_, pUserData_);
				result = 0;
			}
		}
	}
	return result;
}

int __stdcall TS_CloseEndpoint(unsigned long long ullInst_, const char * pEndpoint_)
{
	int result = -1;
	std::lock_guard<std::mutex> lock(g_mutex4ServerList);
	if (!g_serverList.empty()) {
		IocpTcpServerList::iterator iter = g_serverList.find(ullInst_);
		if (iter != g_serverList.end()) {
			ts_new::IocpTcpServer * pServer = iter->second;
			if (pServer) {
				result = pServer->Close(pEndpoint_);
			}
		}
	}
	return result;
}

int __stdcall TS_GetPort(unsigned long long ullInst_, unsigned short & usPort_)
{
	int result = -1;
	std::lock_guard<std::mutex> lock(g_mutex4ServerList);
	if (!g_serverList.empty()) {
		IocpTcpServerList::iterator iter = g_serverList.find(ullInst_);
		if (iter != g_serverList.end()) {
			ts_new::IocpTcpServer * pServer = iter->second;
			if (pServer) {
				usPort_ = pServer->GetPort();
				result = 0;
			}
		}
	}
	return result;
}

int __stdcall TS_SetLogType(unsigned long long ullInst_, unsigned short usLogType_)
{
	int result = -1;
	std::lock_guard<std::mutex> lock(g_mutex4ServerList);
	if (!g_serverList.empty()) {
		IocpTcpServerList::iterator iter = g_serverList.find(ullInst_);
		if (iter != g_serverList.end()) {
			ts_new::IocpTcpServer * pServer = iter->second;
			if (pServer) {
				pServer->SetLogType(usLogType_);
				result = 0;
			}
		}
	}
	return result;
}



