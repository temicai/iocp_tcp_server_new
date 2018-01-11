#include "tcp_server_concrete.h"
#include <sstream>

ts_new::IocpTcpServer::IocpTcpServer(const char * pRootDir_)
{
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
	srand((unsigned int)time(NULL));
	m_hIocp = NULL;
	m_bInit = true;
	m_nRun = false;
	m_usPort = 0;
	m_fdListner = 0;
	m_lpFnAcceptEx = NULL;
	m_msgCb = NULL;
	m_pUserData = NULL;
	SYSTEM_INFO sysInfo;
	GetSystemInfo(&sysInfo);
	m_uiProcessorCount = (unsigned int)sysInfo.dwNumberOfProcessors;
	m_uiWorkerCount = m_uiProcessorCount;// +2;
	m_uiTimeout = ts_new::kIdleLimitTime;
	m_hAcceptEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	m_ullLogInst = 0;
	m_usLogType = pf_logger::eLOGTYPE_FILE;
	m_thdWorkers = new std::thread[m_uiWorkerCount];

	char szPath[256] = { 0 };
	if (pRootDir_ && strlen(pRootDir_)) {
		sprintf_s(szPath, sizeof(szPath), "%slog\\", pRootDir_);
	}
	else {
		GetDllDirectoryA(sizeof(szPath), szPath);
		strcat_s(szPath, sizeof(szPath), "log\\");
	}
	CreateDirectoryExA(".\\", szPath, NULL);
	strcat_s(szPath, sizeof(szPath), "IocpTcpServer\\");
	CreateDirectoryExA(".\\", szPath, NULL);
	m_ullLogInst = LOG_Init();
	if (m_ullLogInst) {
		pf_logger::LogConfig conf;
		conf.usLogPriority = pf_logger::eLOGPRIO_ALL;
		conf.usLogType = m_usLogType;
		strcpy_s(conf.szLogPath, sizeof(conf.szLogPath), szPath);
		LOG_SetConfig(m_ullLogInst, conf);
	}
}

ts_new::IocpTcpServer::~IocpTcpServer()
{
	if (m_nRun) {
		Stop();
	}
	m_bInit = false;

	if (m_hAcceptEvent) {
		CloseHandle(m_hAcceptEvent);
		m_hAcceptEvent = NULL;
	}

	if (m_thdWorkers) {
		delete [] m_thdWorkers;
		m_thdWorkers = NULL;
	}

	if (m_ullLogInst) {
		LOG_Release(m_ullLogInst);
		m_ullLogInst = 0;
	}

	WSACleanup();
}

int ts_new::IocpTcpServer::Start(unsigned short usPort_, unsigned int uiIdleTime_)
{
	if (m_nRun) {
		return 0;
	}
	char szLog[256] = { 0 };
	m_hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	SOCKET sock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (sock == INVALID_SOCKET) {
		CloseHandle(m_hIocp);
		m_hIocp = NULL;
		return -1;
	}
	int nReuse = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&nReuse, sizeof(nReuse));
	int nBufSize = 512 * 1024;
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&nBufSize, sizeof(nBufSize));
	setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&nBufSize, sizeof(nBufSize));
	printf("listen socket=%d\n", (int)sock);
		
	struct sockaddr_in srvAddr;
	srvAddr.sin_family = AF_INET;
	srvAddr.sin_port = htons(usPort_);
	srvAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	if (bind(sock, (const sockaddr *)&srvAddr, sizeof(srvAddr)) == SOCKET_ERROR) {
		CloseHandle(m_hIocp);
		m_hIocp = NULL;
		closesocket(sock);
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]bind port=%hu failed, error=%d\r\n",
			__FUNCTION__, __LINE__, usPort_, WSAGetLastError());
		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_FAULT, m_usLogType);
		return -1;
	}
	if (listen(sock, SOMAXCONN) == SOCKET_ERROR) {
		CloseHandle(m_hIocp);
		m_hIocp = NULL;
		closesocket(sock);
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]listen port=%hu failed, error=%d\r\n",
			__FUNCTION__, __LINE__, usPort_, WSAGetLastError());
		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_FAULT, m_usLogType);
		return -1;
	}
	DWORD dwBytes = 0;
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	if (WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GuidAcceptEx), 
		&m_lpFnAcceptEx, sizeof(m_lpFnAcceptEx), &dwBytes, NULL, NULL) == SOCKET_ERROR) {
		CloseHandle(m_hIocp);
		m_hIocp = NULL;
		closesocket(sock);
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]WSAIoctl don't get AcceptEx function point "
			"error=%d\r\n", __FUNCTION__, __LINE__, WSAGetLastError());
		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_FAULT, m_usLogType);
		return -1;
	}
	CreateIoCompletionPort((HANDLE)sock, m_hIocp, (ULONG_PTR)this, 0);

	m_fdListner = sock;
	m_listenSockCtx.init();
	m_listenSockCtx.nRun = 1;
	m_listenSockCtx.sockFd = sock;
	m_listenSockCtx.nSockType = ts_new::SOCK_FOREMAN;
	m_listenSockCtx.ulLastActiveTime = time(NULL);
	sprintf_s(m_listenSockCtx.szEndpoint, sizeof(m_listenSockCtx.szEndpoint), "127.0.0.1:%hu", usPort_);

	bool bInterrupted = false;
	for (unsigned int i = 0; i < ts_new::kMaxPostAcceptNum; i++) {
		IoContext * pIoCtx = new IoContext();
		memset(pIoCtx, 0, sizeof(IoContext));
		pIoCtx->ioType = ts_new::IO_ACCEPT;
		pIoCtx->uiBufLen = ts_new::kMaxBufSize;
		pIoCtx->pBuf = new char[ts_new::kMaxBufSize];
		memset(pIoCtx->pBuf, 0, ts_new::kMaxBufSize);
		if (postAccept(NULL, pIoCtx)) {
			pIoCtx->nPost = 1;
			m_listenSockCtx.ioCtxList.emplace(pIoCtx);
		}
		else {
			sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]post acceptEx failed, sock=%d, port=%hu\r\n",
				__FUNCTION__, __LINE__, (int)sock, usPort_);
			LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_FAULT, m_usLogType);
			delete pIoCtx;
			pIoCtx = NULL;
			CloseHandle(m_hIocp);
			m_hIocp = NULL;
			bInterrupted = true;
			break;
		}
	}

	if (bInterrupted) {
		delete (&m_listenSockCtx);
		return -1;
	}

	m_nRun = 1;
	m_usPort = usPort_;
	m_uiTimeout = uiIdleTime_;
	WSAEventSelect(sock, m_hAcceptEvent, FD_ACCEPT);
	m_thdListener = std::thread(ts_new::StartListenThread, this);
	m_thdSupervisor = std::thread(ts_new::StartSuperviseThread, this);
	for (unsigned int i = 0; i < m_uiWorkerCount; i++) {
		m_thdWorkers[i] = std::thread(StartWorkThread, this);
	}
	sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]start at %hu\r\n", __FUNCTION__, __LINE__, usPort_);
	LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
	
	return 0;
}

int ts_new::IocpTcpServer::Stop()
{
	if (m_nRun) {
		m_nRun = 0;
		char szLog[256] = { 0 };
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]stop at %hu\r\n", __FUNCTION__, __LINE__, m_usPort);
		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);

		SetEvent(m_hAcceptEvent);

		if (m_hIocp) {
			CloseHandle(m_hIocp);
			m_hIocp = NULL;
		}

		m_thdListener.join();
		m_thdSupervisor.join();
		for (unsigned int i = 0; i < m_uiWorkerCount; i++) {
			PostQueuedCompletionStatus(m_hIocp, -1, 0, NULL);
		}
		for (unsigned int i = 0; i < m_uiWorkerCount; i++) {
			m_thdWorkers[i].join();
		}

		m_usPort = 0;
		m_fdListner = 0;
		m_msgCb = NULL;
		m_pUserData = NULL;
		clearSockContextList();
		
		if (m_listenSockCtx.ioCtxList.empty()) {
			std::set<IoContext *>::iterator iter = m_listenSockCtx.ioCtxList.begin();
			while (iter != m_listenSockCtx.ioCtxList.end()) {
				IoContext * pIoCtx = *iter;
				if (pIoCtx) {
					if (pIoCtx->sockFd != INVALID_SOCKET) {
						shutdown(pIoCtx->sockFd, SD_BOTH);
						closesocket(pIoCtx->sockFd);
						pIoCtx->sockFd = INVALID_SOCKET;
					}
					delete pIoCtx;
					pIoCtx = NULL;
				}
				iter = m_listenSockCtx.ioCtxList.erase(iter);
			}

		}
	}
	return 0;
}

int ts_new::IocpTcpServer::SendData(const char * pEndpoint_, const char * pSendData_, 
	unsigned int uiSendDataLen_)
{
	int result = -1;
	if (pEndpoint_ && strlen(pEndpoint_)) {
		char szLog[256] = { 0 };
		std::string strLink = pEndpoint_;
		{
			std::lock_guard<std::mutex> lock(m_mutex4SockCtxList);
			if (!m_sockCtxList.empty()) {
				SockContextList::iterator iter = m_sockCtxList.find(strLink);
				if (iter != m_sockCtxList.end()) {
					SockContext * pSockCtx = iter->second;
					if (pSockCtx) {
						if (pSockCtx->checkTag()) {
							if (pSockCtx->nRun == 0) {
								sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]send %u data to endpoint=%s, "
									"link is pending to close, PostOutRecvCount=%u\r\n", __FUNCTION__, __LINE__,
									uiSendDataLen_, pEndpoint_, pSockCtx->uiPostOutRecvCount);
								LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
							}
							else {
								IoContext * pSendIoCtx = new IoContext();
								pSendIoCtx->init();
								pSendIoCtx->sockFd = pSockCtx->sockFd;
								strcpy_s(pSendIoCtx->szLink, sizeof(pSendIoCtx->szLink), pEndpoint_);
								pSendIoCtx->ioType = ts_new::IO_SEND;
								pSendIoCtx->uiBufLen = uiSendDataLen_;
								
								DWORD dwBytes = 0, dwFlags = 0;
								WSABUF wsaBuf;
								wsaBuf.buf = (char *)pSendData_;
								wsaBuf.len = uiSendDataLen_;
								if (WSASend(pSockCtx->sockFd, &wsaBuf, 1, &dwBytes, dwFlags, &pSendIoCtx->overlapped, 
									NULL) == SOCKET_ERROR) {
									int nErr = WSAGetLastError();
									if (nErr != WSA_IO_PENDING) {
										sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]send %u data to endpoint=%s, "
											"error=%d\n", __FUNCTION__, __LINE__, uiSendDataLen_, pEndpoint_, nErr);
										LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_FAULT, m_usLogType);
										if (m_msgCb) {
											m_msgCb(MSG_LINK_DISCONNECT, (void *)pEndpoint_, m_pUserData);
										}
										sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]connection %d-%s disconnection\n",
											__FUNCTION__, __LINE__, (int)pSockCtx->sockFd, pSockCtx->szEndpoint);
										LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
										delete pSockCtx;
										pSockCtx = NULL;
										m_sockCtxList.erase(iter);
									}
									else {
										result = 0;
										sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]send %u data to endpoint=%s\n",
											__FUNCTION__, __LINE__, uiSendDataLen_, pEndpoint_);
										LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
									}
								}
								else {
									result = 0;
									sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]send %u data to endpoint=%s\n",
										__FUNCTION__, __LINE__, uiSendDataLen_, pEndpoint_);
									LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								}
								//if (send(pSockCtx->sockFd, pSendData_, (int)uiSendDataLen_, 0) == SOCKET_ERROR) {
								//	int nErr = WSAGetLastError();
								//	sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]send %u data to endpoint=%s failed"
								//		", error=%d\r\n", __FUNCTION__, __LINE__, uiSendDataLen_, pEndpoint_, nErr);
								//	LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								//	
								//	if (m_msgCb) {
								//		m_msgCb(MSG_LINK_DISCONNECT, (void *)pEndpoint_, m_pUserData);
								//	}
								//	sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]connection %d-%s disconnect, type=%d\r\n",
								//		__FUNCTION__, __LINE__, (int)pSockCtx->sockFd, pSockCtx->szEndpoint, pSockCtx->nSockType);
								//	LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								//	
								//	delete pSockCtx;
								//	pSockCtx = NULL;
								//	m_sockCtxList.erase(iter);
								//}
								//else {
								//	result = 0;
								//	sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]send %u data to endpoint=%s\r\n",
								//		__FUNCTION__, __LINE__, uiSendDataLen_, pEndpoint_);
								//	LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								//	
								//}
							}
						}
						else {
							m_sockCtxList.erase(iter);
							sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]endpoint=%s, SockContext is invalid\r\n",
								__FUNCTION__, __LINE__, pEndpoint_);
							LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
						}
					}
					else {
						m_sockCtxList.erase(iter);
						sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]endpoint=%s, SockContext is invalid\r\n",
							__FUNCTION__, __LINE__, pEndpoint_);
						LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
					}
				}
				else {
					sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]not find endpoint=%s\r\n",
						__FUNCTION__, __LINE__, pEndpoint_);
					LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_WARN, m_usLogType);
				}
			}
		}
	}
	return result;
}

void ts_new::IocpTcpServer::SetMessageCallback(fMessageCallback msgCb_, void * pUserData_)
{
	m_msgCb = msgCb_;
	m_pUserData = pUserData_;
}

void ts_new::IocpTcpServer::SetLogType(unsigned short usLogType_)
{
	if (m_ullLogInst) {
		if (m_usLogType != usLogType_) {
			pf_logger::LogConfig conf;
			LOG_GetConfig(m_ullLogInst, &conf);
			if (conf.usLogType != usLogType_) {
				conf.usLogType = usLogType_;
				LOG_SetConfig(m_ullLogInst, conf);
			}
			m_usLogType = usLogType_;
		}
	}
}

int ts_new::IocpTcpServer::Close(const char * pEndpoint_)
{
	int result = -1;
	if (pEndpoint_ && strlen(pEndpoint_)) {
		char szLog[256] = { 0 };
		bool bFindLink = false;
		SOCKET sockFd = 0;
		std::string strLink = pEndpoint_;
		{
			std::lock_guard<std::mutex> lock(m_mutex4SockCtxList);
			if (!m_sockCtxList.empty()) {
				SockContextList::iterator iter = m_sockCtxList.find(strLink);
				if (iter != m_sockCtxList.end()) {
					bFindLink = true;
					result = 0;
					SockContext * pSockCtx = iter->second;
					if (pSockCtx) {
						if (pSockCtx->checkTag()) {
							if (pSockCtx->nRun == 1) {
								pSockCtx->nRun = 0;
							}
							if (pSockCtx->uiPostOutRecvCount == 0) {
								if (pSockCtx->sockFd > 0) {
									sockFd = pSockCtx->sockFd;
									shutdown(pSockCtx->sockFd, SD_BOTH);
									closesocket(pSockCtx->sockFd);
									pSockCtx->sockFd = 0;
								}
								if (!pSockCtx->ioCtxList.empty()) {
									std::set<IoContext *>::iterator iter = pSockCtx->ioCtxList.begin();
									while (iter != pSockCtx->ioCtxList.end()) {
										IoContext * pIoCtx = *iter;
										if (pIoCtx) {
											delete pIoCtx;
											pIoCtx = NULL;
										}
										iter = pSockCtx->ioCtxList.erase(iter);
									}
								}
								delete pSockCtx;
								pSockCtx = NULL;
								m_sockCtxList.erase(iter);
								sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]endpoint=%s close\r\n",
									__FUNCTION__, __LINE__, pEndpoint_);
								if (m_ullLogInst) {
									LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								}
							} 
							else {
								sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]endpoint=%s pending to close, "
									"PostOutRecvCount=%u\r\n", __FUNCTION__, __LINE__, pEndpoint_, 
									pSockCtx->uiPostOutRecvCount);
								if (m_ullLogInst) {
									LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								}
							}
						}
						else {
							pSockCtx = NULL;
							m_sockCtxList.erase(iter);
							sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]endpoint=%s, SockContext invalid, "
								"remove from sockCtxList\r\n", __FUNCTION__, __LINE__, pEndpoint_);
							if (m_ullLogInst) {
								LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
							}
						}
					}
					else {
						m_sockCtxList.erase(iter);
					}
				}
			}
		}
		if (!bFindLink) {
			sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]not find endpoint=%s\r\n",
				__FUNCTION__, __LINE__, pEndpoint_);
			if (m_ullLogInst) {
				LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
			}
		}
	}
	return result;
}

unsigned short ts_new::IocpTcpServer::GetPort()
{
	return m_usPort;
}

void ts_new::IocpTcpServer::supervise()
{
	char szLog[256] = { 0 };
	do {
		{
			std::lock_guard<std::mutex> lock(m_mutex4SockCtxList);
			if (!m_sockCtxList.empty()) {
				SockContextList::iterator iter = m_sockCtxList.begin();
				while (iter != m_sockCtxList.end()) {
					unsigned long long ulCurrentTime = (unsigned long long)time(NULL);
					std::string strLink = iter->first;
					SockContext * pSockCtx = iter->second;
					if (pSockCtx) {
						if (pSockCtx->checkTag()) {
							if (pSockCtx->nSockType == ts_new::SOCK_WORKER) {
								unsigned int uiInterval = (unsigned int)(ulCurrentTime - pSockCtx->ulLastActiveTime);
								if (pSockCtx->nLinger == 0) {
									if (uiInterval > (m_uiTimeout + ts_new::kIdelTolerance)) {
										pSockCtx->nLinger = 1;
										sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]change connection %d-%s to linger,"
											" interval=%u, timeout=%u + %d\r\n", __FUNCTION__, __LINE__, (int)pSockCtx->sockFd,
											pSockCtx->szEndpoint, uiInterval, m_uiTimeout, ts_new::kIdelTolerance);
										if (m_ullLogInst) {
											LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
										}
									}
								}
								else {
									unsigned int uiExtraTolerance = min(m_uiTimeout, ts_new::kIdleLimitTime);
									unsigned int uiExtraTimeout = m_uiTimeout + uiExtraTolerance + ts_new::kIdelTolerance;
									if (uiInterval > uiExtraTimeout) {
										//delete 
										sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]connection %d-%s disconnect linger"
											" interval=%u, timeout=%u\r\n", __FUNCTION__, __LINE__, (int)pSockCtx->sockFd,
											pSockCtx->szEndpoint, uiInterval, uiExtraTimeout);
										LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);

										if (pSockCtx->nRun == 1) {
											pSockCtx->nRun = 0;
										}
										if (!pSockCtx->ioCtxList.empty()) {
											std::set<IoContext *>::iterator iter = pSockCtx->ioCtxList.begin();
											std::set<IoContext *>::iterator iter_end = pSockCtx->ioCtxList.end();
											for (; iter != iter_end; iter++) {
												IoContext * pIoCtx = *iter;
												if (pIoCtx) {
													if (pIoCtx->nPost == 1) {
														PostQueuedCompletionStatus(m_hIocp, 0, (ULONG_PTR)this, &pIoCtx->overlapped);
														pIoCtx->nPost = 0;
													}
												}
											}
										}
									}
								}
							}
						}
						else {
							iter = m_sockCtxList.erase(iter);
							continue;
						}
					}
					else {
						iter = m_sockCtxList.erase(iter);
						continue;
					}
					
					iter++;
				}
			}
		}
		int nTickCount = 0;
		while (m_nRun && nTickCount++ > 5) {
			std::this_thread::sleep_for(std::chrono::microseconds(1000));
		}
		if (!m_nRun) {
			break;
		}
	} while (1);
}

void ts_new::IocpTcpServer::doForemanJob()
{
	char szLog[256] = { 0 };
	DWORD nEventCounts = 1;
	HANDLE * pWaitEvents = new HANDLE[nEventCounts];
	if (pWaitEvents) {
		pWaitEvents[0] = m_hAcceptEvent;
		do {
			DWORD dwIndex = WSAWaitForMultipleEvents(nEventCounts, pWaitEvents, FALSE, 5000, FALSE);
			if (!m_nRun || dwIndex == WSA_WAIT_FAILED) {
				break;
			}
			if (dwIndex == WSA_WAIT_TIMEOUT) {
				if (!m_listenSockCtx.ioCtxList.empty()) {
					std::set<IoContext *>::iterator iter = m_listenSockCtx.ioCtxList.begin();
					for (; iter != m_listenSockCtx.ioCtxList.end(); iter++) {
						IoContext * pIoCtx = *iter;
						if (pIoCtx) {
							if (pIoCtx->sockFd == INVALID_SOCKET) {
								memset(pIoCtx->pBuf, 0, pIoCtx->uiBufLen);
								if (postAccept(NULL, pIoCtx)) {
									pIoCtx->nPost = 1;
								}
							}
							else {
								int nSeconds = 0;
								int nLen = sizeof(nSeconds);
								getsockopt(pIoCtx->sockFd, SOL_SOCKET, SO_CONNECT_TIME, (char *)&nSeconds, &nLen);
								if (nSeconds != -1 && nSeconds > 30) {
									shutdown(pIoCtx->sockFd, SD_BOTH);
									closesocket(pIoCtx->sockFd);
									pIoCtx->sockFd = INVALID_SOCKET;
									memset(pIoCtx->pBuf, 0, pIoCtx->uiBufLen);
									if (postAccept(NULL, pIoCtx)) {
										pIoCtx->nPost = 1;
									}
								}
							}
						}
					}
				}
			}
			else {
				dwIndex -= WAIT_OBJECT_0;
				WSANETWORKEVENTS networkEvents;
				if (dwIndex == 0) {
					int nLimit = 0;
					WSAEnumNetworkEvents(m_fdListner, pWaitEvents[0], &networkEvents);
					if (networkEvents.lNetworkEvents & FD_ACCEPT) {
						nLimit = 10;
					}
					for (int i = 0; i < nLimit; i++) {
						ts_new::IoContext * pAcceptIoCtx = new ts_new::IoContext();
						pAcceptIoCtx->init();
						if (postAccept(NULL, pAcceptIoCtx)) {
							pAcceptIoCtx->nPost = 1;
							m_listenSockCtx.ioCtxList.emplace(pAcceptIoCtx);
						}
					}
				}
			}
		} while (1);
		delete[] pWaitEvents;
		pWaitEvents = NULL;
	}
}

void ts_new::IocpTcpServer::doWorkerJob()
{
	ULONG_PTR dwCompletionKey = 0;
	DWORD dwBytesTransferred = 0;
	LPOVERLAPPED pOverlapped;
	char szLog[256] = { 0 };
	do {
		BOOL bRet = GetQueuedCompletionStatus(m_hIocp, &dwBytesTransferred, (PULONG_PTR)&dwCompletionKey, 
			&pOverlapped, WSA_INFINITE);
		if (!bRet) {
			int nErr = GetLastError();
			if (pOverlapped == NULL) {
				if (nErr != WSA_WAIT_TIMEOUT) {
					sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]GetQueuedCompletionStatus error=%d, "
						"break worker thread\n", __FUNCTION__, __LINE__, nErr);
					LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
					break;
				}
				break;
			}
			else { 
				sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]GetQueuedCompletionStatus error=%d, "
					"bytesTransferred=%lu, CompletionKey=%llu\n", __FUNCTION__, __LINE__, nErr, dwBytesTransferred, 
					(unsigned long long)dwCompletionKey);
				LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
				
				ts_new::IoContext * pIoCtx = CONTAINING_RECORD(pOverlapped, ts_new::IoContext, overlapped);
				//memset(pIoCtx->pBuf, 0, pIoCtx->uiBufLen);
				switch (pIoCtx->ioType) {
					case ts_new::IO_ACCEPT: {
						doAccept(dwCompletionKey, dwBytesTransferred, pIoCtx);
						break;
					}
					case ts_new::IO_RECV: {
						doReceive(dwCompletionKey, dwBytesTransferred, pIoCtx);
						break;
					}
					case ts_new::IO_SEND: {
						doSend(dwCompletionKey, dwBytesTransferred, pIoCtx);
						break;
					}
				}
			}
		}
		else {
			if (dwBytesTransferred == -1) { //Post error
				sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]receive stop signal\n",
					__FUNCTION__, __LINE__);
				LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_STATE, m_usLogType);
				break;
			}
			ts_new::IoContext * pIoCtx = CONTAINING_RECORD(pOverlapped, ts_new::IoContext, overlapped);
			switch (pIoCtx->ioType) {
				case ts_new::IO_ACCEPT: {
					doAccept(dwCompletionKey, dwBytesTransferred, pIoCtx);
					break;
				}
				case ts_new::IO_RECV: {
					if (dwBytesTransferred == 0) {
						std::lock_guard<std::mutex> lock(m_mutex4SockCtxList);
						SockContextList::iterator iter = m_sockCtxList.find((std::string)pIoCtx->szLink);
						if (iter != m_sockCtxList.end()) {
							SockContext * pSockCtx = iter->second;
							if (pSockCtx) {
								if (pSockCtx->checkTag()) {
									std::string strEndpoint = pSockCtx->szEndpoint;
									if (pSockCtx->nRun) {
										pSockCtx->nRun = 0;
										sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]connection %d-%s change run status 1->0\r\n",
											__FUNCTION__, __LINE__, (int)pSockCtx->sockFd, pSockCtx->szEndpoint);
										LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
									}
									if (pSockCtx->uiPostOutRecvCount > 0) {
										pSockCtx->uiPostOutRecvCount -= 1;
										sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]connection %d-%s change PostOutRecvCount"
											" to %u\r\n", __FUNCTION__, __LINE__, (int)pSockCtx->sockFd, pSockCtx->szEndpoint,
											pSockCtx->uiPostOutRecvCount);
										LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
									}
									if (pSockCtx->uiPostOutRecvCount == 0) {
										m_sockCtxList.erase(iter);
										sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]connection %d-%s remove from "
											"SocketContextList\r\n", __FUNCTION__, __LINE__, (int)pSockCtx->sockFd, pSockCtx->szEndpoint);
										LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
										if (m_msgCb) {
											m_msgCb(MSG_LINK_DISCONNECT, (void *)strEndpoint.c_str(), m_pUserData);
										}
										sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]connection %d-%s disconnection, type=%d\r\n",
											__FUNCTION__, __LINE__, (int)pSockCtx->sockFd, pSockCtx->szEndpoint, pSockCtx->nSockType);
										LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
										delete pSockCtx;
										pSockCtx = NULL;
									}
								}
								else {
									m_sockCtxList.erase(iter);
								}
							}
						}
					}
					else {
						doReceive(dwCompletionKey, dwBytesTransferred, pIoCtx);
					}
					break;
				}
				case ts_new::IO_SEND: {
					doSend(dwCompletionKey, dwBytesTransferred, pIoCtx);
					break;
				}
			}
		}
	} while (1);
}

void ts_new::IocpTcpServer::addSockcontext(std::string strEndpoint_, SockContext * pSockCtx_)
{
	if (pSockCtx_ && !strEndpoint_.empty()) {
		std::lock_guard<std::mutex> lock(m_mutex4SockCtxList);
		SockContextList::iterator iter = m_sockCtxList.find(strEndpoint_);
		if (iter != m_sockCtxList.end()) {
			SockContext * pDstSockCtx = iter->second;
			if (pDstSockCtx) {
				delete pDstSockCtx;
				pDstSockCtx = NULL;
			}
			m_sockCtxList[strEndpoint_] = pSockCtx_;
		}
		else {
			m_sockCtxList.emplace(strEndpoint_, pSockCtx_);
		}
	}
}

void ts_new::IocpTcpServer::clearSockContextList()
{
	std::lock_guard<std::mutex> lock(m_mutex4SockCtxList);
	if (!m_sockCtxList.empty()) {
		SockContextList::iterator iter = m_sockCtxList.begin();
		while (iter != m_sockCtxList.end()) {
			SockContext * pSockCtx = iter->second;
			if (pSockCtx) {
				delete pSockCtx;
				pSockCtx = NULL;
			}
			iter = m_sockCtxList.erase(iter);
		}
	}
}

void ts_new::IocpTcpServer::doAccept(ULONG_PTR dwKey_, DWORD dwBytesTransfered_, ts_new::IoContext * pIoCtx_)
{
	char szLog[256] = { 0 };
	if (pIoCtx_) {
		setsockopt(pIoCtx_->sockFd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&m_fdListner, sizeof(m_fdListner));
		unsigned long long ulCurrentTime = (unsigned long long)time(NULL);
		sockaddr_in remoteAddr;
		//memset(&remoteAddr, 0, sizeof(remoteAddr));
		int nAddrSize = sizeof(remoteAddr);
		int nRet = getpeername(pIoCtx_->sockFd, (sockaddr *)&remoteAddr, &nAddrSize);
		if (nRet == SOCKET_ERROR) {
			int nErr = WSAGetLastError();
			printf("getpeername=%d\n", nErr);
		}
		char szClientIp[20] = { 0 };
		unsigned short usClientPort = ntohs(remoteAddr.sin_port);
		inet_ntop(AF_INET, &remoteAddr.sin_addr, szClientIp, sizeof(szClientIp));

		ts_new::SockContext * pNewSockCtx = new ts_new::SockContext();
		pNewSockCtx->init();
		pNewSockCtx->nRun = 1;
		pNewSockCtx->nSockType = ts_new::SOCK_WORKER;
		sprintf_s(pNewSockCtx->szEndpoint, sizeof(pNewSockCtx->szEndpoint), "%s:%hu", szClientIp, usClientPort);
		pNewSockCtx->sockFd = pIoCtx_->sockFd;
		pNewSockCtx->ulLastActiveTime = ulCurrentTime;
		std::string strNewEndpoint = pNewSockCtx->szEndpoint;
		addSockcontext(strNewEndpoint, pNewSockCtx);
		CreateIoCompletionPort((HANDLE)pNewSockCtx->sockFd, m_hIocp, (ULONG_PTR)this, 0);
		//post receive for a new incoming connection
		//for (unsigned int i = 0; i < m_uiProcessorCount; i++) {
		//	IoContext * pNewIoCtx = new IoContext();
		//	memset(pNewIoCtx, 0, sizeof(IoContext));
		//	pNewIoCtx->ioType = ts_new::IO_RECV;
		//	pNewIoCtx->sockFd = pNewSockCtx->sockFd;
		//	pNewIoCtx->uiBufLen = ts_new::kMaxBufSize;
		//	pNewIoCtx->pBuf = new char[pNewIoCtx->uiBufLen];
		//	memset(pNewIoCtx->pBuf, 0, pNewIoCtx->uiBufLen);
		//	pNewSockCtx->ioCtxList.emplace(pNewIoCtx);
		//	if (!postReceive(pNewSockCtx, pNewIoCtx)) {
		//		break;
		//	}
		//}
		
		IoContext * pRecvIoCtx = new IoContext();
		memset(pRecvIoCtx, 0, sizeof(IoContext));
		pRecvIoCtx->ioType = ts_new::IO_RECV;
		pRecvIoCtx->sockFd = pNewSockCtx->sockFd;
		pRecvIoCtx->uiBufLen = ts_new::kMaxBufSize;
		pRecvIoCtx->pBuf = new char[pRecvIoCtx->uiBufLen];
		memset(pRecvIoCtx->pBuf, 0, pRecvIoCtx->uiBufLen);
		strcpy_s(pRecvIoCtx->szLink, sizeof(pRecvIoCtx->szLink), pNewSockCtx->szEndpoint);
		if (!postReceive(NULL, pRecvIoCtx)) {

		}
		else {
			pNewSockCtx->ioCtxList.emplace(pRecvIoCtx);
		}
		
		if (m_msgCb) {
			m_msgCb(MSG_LINK_CONNECT, (void *)strNewEndpoint.c_str(), m_pUserData);
		}
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]new connection %d-%s, sockctx=%llu\r\n",
			__FUNCTION__, __LINE__, (int)pNewSockCtx->sockFd, strNewEndpoint.c_str(), (unsigned long long)pNewSockCtx);
		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
		if (dwBytesTransfered_ > 0) {
			MessageContent msg;
			strcpy_s(msg.szEndPoint, sizeof(msg.szEndPoint), strNewEndpoint.c_str());
			msg.ulMsgTime = ulCurrentTime;
			msg.uiMsgDataLen = (unsigned int)dwBytesTransfered_;
			msg.pMsgData = new unsigned char[msg.uiMsgDataLen + 1];
			memcpy_s(msg.pMsgData, msg.uiMsgDataLen + 1, pIoCtx_->pBuf, dwBytesTransfered_);
			msg.pMsgData[msg.uiMsgDataLen] = '\0';
			if (m_msgCb) {
				m_msgCb(MSG_DATA, (void *)&msg, m_pUserData);
			}
			sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]receive %lu data from connection %d-%s, "
				"at time=%llu\r\n", __FUNCTION__, __LINE__, dwBytesTransfered_, (int)pIoCtx_->sockFd, msg.szEndPoint, 
				ulCurrentTime);
			LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
			delete [] msg.pMsgData;
			msg.pMsgData = NULL;
		}
		//post accept
		pIoCtx_->nPost = 0;
		memset(pIoCtx_->pBuf, 0, pIoCtx_->uiBufLen);
		if (postAccept(NULL, pIoCtx_)) {
			pIoCtx_->nPost = 1;
		}
	}
}

void ts_new::IocpTcpServer::doReceive(ULONG_PTR dwKey_, DWORD dwBytesTransfered_, ts_new::IoContext * pIoCtx_)
{
	if (pIoCtx_) {
		char szLog[256] = { 0 };
		bool bPost = false;
		unsigned long long ulCurrentTime = (unsigned long long)time(NULL);
		{
			std::lock_guard<std::mutex> lock(m_mutex4SockCtxList);
			SockContextList::iterator iter = m_sockCtxList.find((std::string)pIoCtx_->szLink);
			if (iter != m_sockCtxList.end()) {
				SockContext * pSockCtx = iter->second;
				if (pSockCtx) {
					if (pSockCtx->checkTag()) {
						pSockCtx->ulLastActiveTime = ulCurrentTime;
						pIoCtx_->nPost = 0;
						if (pSockCtx->nLinger == 1) {
							pSockCtx->nLinger = 0;
							sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]change connection %d-%s not linger\r\n",
								__FUNCTION__, __LINE__, (int)pSockCtx->sockFd, pSockCtx->szEndpoint);
							LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
						}
						if (pSockCtx->uiPostOutRecvCount > 0) {
							pSockCtx->uiPostOutRecvCount -= 1;
						}
						if (dwBytesTransfered_ > 0) {
							//if (pIoCtx_->uiSequence == pSockCtx->uiCurrentReadSequence) {
							//pSockCtx->uiCurrentReadSequence++;
							MessageContent msg;
							msg.uiMsgDataLen = (unsigned int)dwBytesTransfered_;
							msg.pMsgData = new unsigned char[msg.uiMsgDataLen + 1];
							memcpy_s(msg.pMsgData, msg.uiMsgDataLen + 1, pIoCtx_->pBuf, dwBytesTransfered_);
							msg.pMsgData[msg.uiMsgDataLen] = '\0';
							msg.ulMsgTime = ulCurrentTime;
							strcpy_s(msg.szEndPoint, sizeof(msg.szEndPoint), pSockCtx->szEndpoint);
							if (m_msgCb) {
								m_msgCb(MSG_DATA, (void *)&msg, m_pUserData);
							}
							sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]receive %lu data from connection %d-%s, "
								"at time=%llu\r\n", __FUNCTION__, __LINE__, dwBytesTransfered_, (int)pSockCtx->sockFd,
								pSockCtx->szEndpoint, ulCurrentTime);
							LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
							delete[] msg.pMsgData;
							msg.pMsgData = NULL;
							//else if (pIoCtx_->uiSequence > pSockCtx->uiCurrentReadSequence) {
							//	ts_new::LingerData * pLingerData = new ts_new::LingerData();
							//	memset(pLingerData, 0, sizeof(ts_new::LingerData));
							//	pLingerData->uiDataLen = dwBytesTransfered_;
							//	pLingerData->pData = new char[pLingerData->uiDataLen + 1];
							//	memcpy_s(pLingerData->pData, pLingerData->uiDataLen + 1, pIoCtx_->pBuf, dwBytesTransfered_);
							//	pLingerData->pData[pLingerData->uiDataLen] = '\0';
							//	pSockCtx->lingerDataList.emplace(std::make_pair(pIoCtx_->uiSequence, pLingerData));
							//}
						}
						if (pSockCtx->nRun) {
							bPost = true;
						}
						else {
							if (pSockCtx->uiPostOutRecvCount == 0) {
								m_sockCtxList.erase(iter);
								if (m_msgCb) {
									m_msgCb(MSG_LINK_DISCONNECT, (void *)pSockCtx->szEndpoint, m_pUserData);
								}
								sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]connection %d-%s disconnect, "
									"type=%d\r\n", __FUNCTION__, __LINE__, (int)pSockCtx->sockFd, pSockCtx->szEndpoint,
									pSockCtx->nSockType);
								LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);

								delete pSockCtx;
								pSockCtx = NULL;
							}
						}
					}
					else {
						m_sockCtxList.erase(iter);
					}
				}
			}
		}

		if (bPost) {
			memset(pIoCtx_->pBuf, 0, pIoCtx_->uiBufLen);
			postReceive(NULL, pIoCtx_);
		}
	}
}

void ts_new::IocpTcpServer::doSend(ULONG_PTR dwKey_, DWORD dwBytesTransfered_, ts_new::IoContext * pIoCtx_)
{
	if (pIoCtx_) {
		char szLog[256] = { 0 };
		std::lock_guard<std::mutex> lock(m_mutex4SockCtxList);
		SockContextList::iterator iter = m_sockCtxList.find((std::string)pIoCtx_->szLink);
		if (iter != m_sockCtxList.end()) {
			SockContext * pSockCtx = iter->second;
			if (pSockCtx) {
				if (pSockCtx->checkTag()) {
					if (pSockCtx->nRun) {
						pSockCtx->ulLastActiveTime = (unsigned long long)time(NULL);
					}
					sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]send %lu data to connection %d-%s, type=%d\n",
						__FUNCTION__, __LINE__, dwBytesTransfered_, (int)pSockCtx->sockFd, pSockCtx->szEndpoint,
						pSockCtx->nSockType);
					LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
				}
			}
		}
		delete pIoCtx_;
		pIoCtx_ = NULL;
	}
}

bool ts_new::IocpTcpServer::postAccept(ts_new::SockContext * pSockCtx_, ts_new::IoContext * pAcceptIoCtx_)
{
	bool result = false;
	if (pAcceptIoCtx_) {
		char szLog[256] = { 0 };
		//if (pSockCtx_->checkTag()) {
		//	if (pSockCtx_->nRun) {
		//		DWORD dwBytes = 0;
		//		WSABUF wsaBuf;
		//		wsaBuf.buf = pAcceptIoCtx_->pBuf;
		//		wsaBuf.len = pAcceptIoCtx_->uiBufLen;
		//		pAcceptIoCtx_->ioType = ts_new::IO_ACCEPT;
		//		pAcceptIoCtx_->sockFd = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
		//		printf("postAccept sock=%d\n", (int)pAcceptIoCtx_->sockFd);
		//		if (pAcceptIoCtx_->sockFd != INVALID_SOCKET) {
		//			//pAcceptIoCtx_->sockFd = fd;
		//			if (!m_lpFnAcceptEx(pSockCtx_->sockFd, pAcceptIoCtx_->sockFd, wsaBuf.buf, 
		//				//wsaBuf.len - ((sizeof(sockaddr_in) + 16) * 2), (sizeof(sockaddr_in) + 16), (sizeof(sockaddr_in) + 16),
		//				//change: no more wait for data coming when accept new connection
		//				0, (sizeof(sockaddr_in) + 16), (sizeof(sockaddr_in) + 16),
		//				&dwBytes, &pAcceptIoCtx_->overlapped)) {
		//				int nErr = WSAGetLastError();
		//				if (nErr != WSA_IO_PENDING) {
		//					sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]AcceptEx failed with error=%d\r\n",
		//						__FUNCTION__, __LINE__, nErr);
		//					LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
		//					
		//					shutdown(pAcceptIoCtx_->sockFd, SD_BOTH);
		//					closesocket(pAcceptIoCtx_->sockFd);
		//					pAcceptIoCtx_->sockFd = 0;
		//				}
		//				else {
		//					result = true;
		//				}
		//			}
		//			else {
		//				PostQueuedCompletionStatus(m_hIocp, 0, (ULONG_PTR)this, &pAcceptIoCtx_->overlapped);
		//				result = true;
		//			}
		//		}
		//	}
		//	else {
		//		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]no more postAccept because socket not run\r\n",
		//			__FUNCTION__, __LINE__);
		//		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
		//	}
		//}

		DWORD dwBytes = 0;
		WSABUF wsaBuf;
		wsaBuf.len = pAcceptIoCtx_->uiBufLen;
		wsaBuf.buf = pAcceptIoCtx_->pBuf;
		pAcceptIoCtx_->ioType = ts_new::IO_ACCEPT;
		pAcceptIoCtx_->sockFd = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
		if (!m_lpFnAcceptEx(m_fdListner, pAcceptIoCtx_->sockFd, wsaBuf.buf, 0, (sizeof(sockaddr_in) + 16),
			(sizeof(sockaddr_in) + 16), &dwBytes, &pAcceptIoCtx_->overlapped)) {
			int nErr = WSAGetLastError();
			if (nErr == WSA_IO_PENDING) {
				result = true;
			}
			else {
				sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]acceptEx failed with error=%d\n",
					__FUNCTION__, __LINE__, nErr);
				LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_FAULT, m_usLogType);
			}
		}
		else {
			PostQueuedCompletionStatus(m_hIocp, 0, (ULONG_PTR)this, &pAcceptIoCtx_->overlapped);
			result = true;
		}
	}
	return result;
}

bool ts_new::IocpTcpServer::postReceive(ts_new::SockContext * pSockCtx_, ts_new::IoContext * pRecvIoCtx_)
{
	bool result = false;
	if (pRecvIoCtx_) {
		DWORD dwBytes = 0, dwFlags = 0;
		WSABUF wsaBuf;
		wsaBuf.buf = pRecvIoCtx_->pBuf;
		wsaBuf.len = pRecvIoCtx_->uiBufLen;
		pRecvIoCtx_->ioType = ts_new::IO_RECV;
		if (WSARecv(pRecvIoCtx_->sockFd, &wsaBuf, 1, &dwBytes, &dwFlags, &pRecvIoCtx_->overlapped, 
			NULL) == SOCKET_ERROR) {
			int nErr = WSAGetLastError();
			if (nErr != WSA_IO_PENDING) {
				char szLog[256] = { 0 };
				sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]connection %d-%s recv error =%d\n", __FUNCTION__, 
					__LINE__, (int)pRecvIoCtx_->sockFd, pRecvIoCtx_->szLink, WSAGetLastError());
				LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
				return false;
			}
		}
		result = true;

		
		std::lock_guard<std::mutex> lock(m_mutex4SockCtxList);
		SockContextList::iterator iter = m_sockCtxList.find((std::string)pRecvIoCtx_->szLink);
		if (iter != m_sockCtxList.end()) {
			SockContext * pSockCtx = iter->second;
			if (pSockCtx) {
				if (pSockCtx->checkTag() && pSockCtx->nRun) {
					pRecvIoCtx_->uiSequence = pSockCtx->uiCurrentReadSequence++;
					pSockCtx->uiPostOutRecvCount++;
					pRecvIoCtx_->nPost = 1;
				}
			}
		}
		
	}
	return result;
}

void ts_new::StartSuperviseThread(void * param_)
{
	ts_new::IocpTcpServer * pServer = (ts_new::IocpTcpServer *)param_;
	if (pServer) {
		char szLog[256] = { 0 };
		std::stringstream ss;
		ss << std::this_thread::get_id();
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]supervise thread %s start\r\n",
			__FUNCTION__, __LINE__, ss.str().c_str());
		if (pServer->m_ullLogInst) {
			LOG_Log(pServer->m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, pServer->m_usLogType);
		}
		pServer->supervise();
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]supervise thread %s stop\r\n",
			__FUNCTION__, __LINE__, ss.str().c_str());
		if (pServer->m_ullLogInst) {
			LOG_Log(pServer->m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, pServer->m_usLogType);
		}
	}
}

void ts_new::StartListenThread(void * param_)
{
	ts_new::IocpTcpServer * pServer = (ts_new::IocpTcpServer *)param_;
	if (pServer) {
		char szLog[256] = { 0 };
		std::stringstream ss;
		ss << std::this_thread::get_id();
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]listen thread %s start\r\n", 
			__FUNCTION__, __LINE__, ss.str().c_str());
		if (pServer->m_ullLogInst) {
			LOG_Log(pServer->m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, pServer->m_usLogType);
		}
		pServer->doForemanJob();
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]listen thread %s stop\r\n",
			__FUNCTION__, __LINE__, ss.str().c_str());
		if (pServer->m_ullLogInst) {
			LOG_Log(pServer->m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, pServer->m_usLogType);
		}
	}
}

void ts_new::StartWorkThread(void * param_)
{
	ts_new::IocpTcpServer * pServer = (ts_new::IocpTcpServer *)param_;
	if (pServer) {
		char szLog[256] = { 0 };
		std::stringstream ss; 
		ss << std::this_thread::get_id();
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]worker thread %s start\r\n", 
			__FUNCTION__, __LINE__, ss.str().c_str());
		if (pServer->m_ullLogInst) {
			LOG_Log(pServer->m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, pServer->m_usLogType);
		}
		pServer->doWorkerJob();
		sprintf_s(szLog, sizeof(szLog), "[IocpTcpServer]%s[%d]worker thread %s stop\r\n", 
			__FUNCTION__, __LINE__, ss.str().c_str());
		if (pServer->m_ullLogInst) {
			LOG_Log(pServer->m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, pServer->m_usLogType);
		}
	}
}

