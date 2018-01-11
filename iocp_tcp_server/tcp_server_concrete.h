#ifndef TCP_SERVER_CONCRETE_H_8C9772F0763B4EDDA2BE5271F55A76D6
#define TCP_SERVER_CONCRETE_H_8C9772F0763B4EDDA2BE5271F55A76D6

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <mstcpip.h>
#include <time.h>
#include <map>
#include <string>
#include <queue>
#include <set>
#include <functional>
#include <mutex>
#include <thread>
#include <condition_variable>

#include "tcp_common.h"
#include "pf_log.h"

#ifdef _DEBUG

#include "vld.h"
#pragma comment(lib, "vld.lib")
#endif


namespace ts_new
{
	const int kMaxPostAcceptNum = 10;
	const int kMaxBufSize = 512 * 1024;
	const int kMaxIoNum = 64;
	const int kIdleLimitTime = 60;
	const int kIdelTolerance = 10;

	enum IoType
	{
		IO_UNDEFAULT = 0,
		IO_ACCEPT = 1,
		IO_RECV = 2,
		IO_SEND = 3
	};

	enum SockType
	{
		SOCK_UNDEFAULT = 0,
		SOCK_FOREMAN = 1,
		SOCK_WORKER = 2,
	};

	typedef struct tagIoContext
	{
		OVERLAPPED overlapped;
		SOCKET sockFd;
		char * pBuf;
		unsigned int uiBufLen;
		int nPost;//default 0, if operate post, set 1;
		IoType ioType;
		unsigned int uiSequence;
		char szLink[32];
		tagIoContext()
		{
			init();
		}
		~tagIoContext()
		{
			if (pBuf && uiBufLen > 0) {
				uiBufLen = 0;
				delete[] pBuf;
				pBuf = NULL;
			}
		}
		void init()
		{
			sockFd = INVALID_SOCKET;
			pBuf = NULL;
			uiBufLen = 0;
			ioType = IO_UNDEFAULT;
			nPost = 0;
			uiSequence = 0;
			memset(&overlapped, 0, sizeof(OVERLAPPED));
			memset(szLink, 0, sizeof(szLink));
		}
	} IoContext;

	typedef struct tagLingerData
	{
		char * pData;
		unsigned int uiDataLen;
		tagLingerData()
		{
			pData = NULL;
			uiDataLen = 0;
		}
		~tagLingerData()
		{
			if (pData && uiDataLen) {
				delete[] pData;
				pData = NULL;
				uiDataLen = 0;
			}
		}
	} LingerData;

	typedef std::pair<unsigned int, LingerData *> LingerDataPair;

	typedef std::set<LingerDataPair> LingerDataPairList;

	typedef struct tagSockContext
	{
		SOCKET sockFd;
		int nSockType:16; //SockType
		int nRun:8; //0:false, 1:true
		int nLinger:8; //if timeout, set linger(1), default 0; 0:false, 1:true
		int nTag;
		unsigned int uiPostOutRecvCount;
		unsigned long long ulLastActiveTime;
		unsigned int uiReadSequence;
		unsigned int uiCurrentReadSequence;
		char szEndpoint[32];
		std::set<IoContext *> ioCtxList;
		LingerDataPairList lingerDataList;
		tagSockContext()
		{
			init();
		}
		~tagSockContext()
		{
			if (checkTag()) {
				if (sockFd > 0) {
					shutdown(sockFd, SD_BOTH);
					closesocket(sockFd);
					sockFd = 0;
				}
				if (!ioCtxList.empty()) {
					std::set<IoContext *>::iterator iter = ioCtxList.begin();
					while (iter != ioCtxList.end()) {
						IoContext * pIoCtx = *iter;
						if (pIoCtx) {
							delete pIoCtx;
							pIoCtx = NULL;
						}
						iter = ioCtxList.erase(iter);
					}
				}
				if (!lingerDataList.empty()) {
					LingerDataPairList::iterator iter = lingerDataList.begin();
					LingerDataPairList::iterator iter_end = lingerDataList.end();
					while (iter != iter_end) {
						LingerData * pLingerData = iter->second;
						if (pLingerData) {
							delete pLingerData;
							pLingerData = NULL;
						}
						iter = lingerDataList.erase(iter);
					}
				}
				nTag = 0;
			}
		}
		bool checkTag()
		{
			return (nTag == 0xabcd);
		}
		void init()
		{
			nTag = 0xabcd;
			sockFd = INVALID_SOCKET;
			nSockType = 0;
			nRun = 0;
			nLinger = 0;
			uiPostOutRecvCount = 0;
			ulLastActiveTime = 0;
			uiReadSequence = 0;
			uiCurrentReadSequence = 0;
			ioCtxList.clear();
			lingerDataList.clear();
			memset(szEndpoint, 0, sizeof(szEndpoint));
		}
	} SockContext;

	typedef std::map<std::string, SockContext *> SockContextList;

	class IocpTcpServer
	{
	public:
		IocpTcpServer(const char * pRootDir);
		~IocpTcpServer();
		int Start(unsigned short usPort, unsigned int uiIdleTime);
		int Stop();
		int SendData(const char * pEndpoint, const char * pSendData, unsigned int uiSendDataLen);
		void SetMessageCallback(fMessageCallback fMsgCb, void * pUserData);
		void SetLogType(unsigned short);
		int Close(const char * pEndpoint);
		unsigned short GetPort();
		friend void StartSuperviseThread(void * param);
		friend void StartWorkThread(void * param);
		friend void StartListenThread(void * param);

	private:
		bool m_bInit;
		bool m_nRun;
		unsigned short m_usPort;
		SOCKET m_fdListner;
		fMessageCallback m_msgCb;
		void * m_pUserData;
		unsigned long long m_ullLogInst;
		unsigned short m_usLogType;
		HANDLE m_hIocp;
		LPFN_ACCEPTEX m_lpFnAcceptEx;
		unsigned int m_uiTimeout;
		SockContext m_listenSockCtx;
		SockContextList m_sockCtxList;
		mutable std::mutex m_mutex4SockCtxList;
		std::thread m_thdSupervisor;
		std::thread m_thdListener;
		std::thread * m_thdWorkers;
		unsigned int m_uiWorkerCount;
		unsigned int m_uiProcessorCount;
		HANDLE m_hAcceptEvent;
	protected:
		void supervise();
		void doForemanJob();
		void doWorkerJob();
		void addSockcontext(std::string, SockContext *);
		void clearSockContextList();
		void doAccept(ULONG_PTR, DWORD, IoContext *);
		void doReceive(ULONG_PTR, DWORD, IoContext *);
		void doSend(ULONG_PTR, DWORD, IoContext *);
		bool postAccept(SockContext *, IoContext *);
		bool postReceive(SockContext *, IoContext *);
	};

}


#endif
