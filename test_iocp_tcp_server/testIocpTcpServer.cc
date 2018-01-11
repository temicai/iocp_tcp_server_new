#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <queue>
#include <condition_variable>

#include "iocp_tcp_server.h"
#pragma comment(lib, "iocp_tcp_server.lib")

typedef struct tagReplyParam
{
	char szEndpoint[20];
	char * pReplyData;
	unsigned int uiReplyDataLen;
	tagReplyParam()
	{
		init();
	}
	void init()
	{
		pReplyData = NULL;
		uiReplyDataLen = 0;
		memset(szEndpoint, 0, sizeof(szEndpoint));
	}
	~tagReplyParam()
	{
		if (pReplyData && uiReplyDataLen > 0) {
			delete[] pReplyData;
			pReplyData = NULL;
			uiReplyDataLen = 0;
		}
	}
} ReplyParam;


unsigned long long g_ullServerInst = 0;
std::mutex g_mutex4LinkList;
std::vector<std::string> g_linkList;
int nCount = 0;
std::queue<ReplyParam *> g_replyQue;
std::condition_variable g_cond4ReplyQue;
std::mutex g_mutex4ReplyQue;
bool g_bRun = false;

bool addReply(ReplyParam * pReply)
{
	if (pReply) {
		std::lock_guard<std::mutex> lock(g_mutex4ReplyQue);
		g_replyQue.emplace(pReply);
		if (g_replyQue.size() == 1) {
			g_cond4ReplyQue.notify_one();
		}
		return true;
	}
	return false;
}

void dealReply()
{
	do {
		ReplyParam * pReply = NULL;
		{
			std::unique_lock<std::mutex> lock(g_mutex4ReplyQue);
			g_cond4ReplyQue.wait(lock, [&] {
				return (!g_replyQue.empty() || !g_bRun);
			});
			if (g_replyQue.empty() && !g_bRun) {
				break;
			}
			pReply = g_replyQue.front();
			g_replyQue.pop();
		}
		if (pReply) {
			if (g_ullServerInst) {
				int n = TS_SendData(g_ullServerInst, pReply->szEndpoint, pReply->pReplyData, pReply->uiReplyDataLen);
				printf("send reply %u data to %s: %d\n", pReply->uiReplyDataLen, pReply->szEndpoint, n);
			}
			delete pReply;
			pReply = NULL;
		}
	} while (1);
}

static void __stdcall msgCb(int nType_, void * pMsg_, void * pUserData_)
{
	switch (nType_) {
		case MSG_LINK_CONNECT: {
			std::string strEndpoint = (char *)pMsg_;
			printf("link connect: %s\n", strEndpoint.c_str());
			std::lock_guard<std::mutex> lk(g_mutex4LinkList);
			g_linkList.emplace_back(strEndpoint);
			break;
		}
		case MSG_LINK_DISCONNECT: {
			std::string strEndpoint = (char *)pMsg_;
			printf("link disconnect: %s\n", strEndpoint.c_str());
			std::lock_guard<std::mutex> lk(g_mutex4LinkList);
			std::vector<std::string>::iterator iter = std::find(g_linkList.begin(), g_linkList.end(), strEndpoint);
			if (iter != g_linkList.end()) {
				g_linkList.erase(iter);
			}
			break;
		}
		case MSG_DATA: {
			MessageContent * pMsg = (MessageContent *)pMsg_;
			printf("receive %u data from link: %s at time: %llu\n", pMsg->uiMsgDataLen, pMsg->szEndPoint, pMsg->ulMsgTime);
			if (g_ullServerInst > 0) {
				char * pRecvData = new char[pMsg->uiMsgDataLen + 1];
				memcpy_s(pRecvData, pMsg->uiMsgDataLen + 1, pMsg->pMsgData, pMsg->uiMsgDataLen);
				pRecvData[pMsg->uiMsgDataLen] = '\0';
				printf("recv %s\n", pRecvData);
				ReplyParam * pReply = new ReplyParam();
				pReply->init();
				strcpy_s(pReply->szEndpoint, sizeof(pReply->szEndpoint), pMsg->szEndPoint);
				pReply->uiReplyDataLen = pMsg->uiMsgDataLen;
				pReply->pReplyData = new char[pReply->uiReplyDataLen + 1];
				memcpy_s(pReply->pReplyData, pReply->uiReplyDataLen + 1, pRecvData, pReply->uiReplyDataLen);
				pReply->pReplyData[pReply->uiReplyDataLen] = '\0';
				if (!addReply(pReply)) {
					delete pReply;
					pReply = NULL;
				}
				delete [] pRecvData;
				pRecvData = NULL;
			}
			break;
		}
	}
}

int main(int argc, char ** argv)
{
	srand((unsigned int)time(NULL));
	unsigned short usPort = 20000;
	if (argc > 1) {
		usPort = atoi(argv[1]);
	}
	g_ullServerInst = TS_StartServer(usPort, msgCb, NULL, 30);
	if (g_ullServerInst > 0) {
		printf("start at %hu\n", usPort);
		TS_SetLogType(g_ullServerInst, 0);
		g_bRun = true;
		std::thread thd_reply(dealReply);
		while (1) {
			char c = 0;
			scanf_s("%c", &c, 1);
			if (c == 'q') {
				g_bRun = false;
				break;
			}
			else if (c == 's') {
				std::lock_guard<std::mutex> lock(g_mutex4LinkList);
				int nCount = (int)g_linkList.size();
				if (nCount > 0) {
					int nIndex = rand() % nCount;
					std::string strEndpoint = g_linkList[nIndex];
					char szMsg[64] = { 0 };
					sprintf_s(szMsg, sizeof(szMsg), "hello world, %llu\n", (unsigned long long)time(NULL));
					unsigned int uiMsgLen = (unsigned int)strlen(szMsg);
					int nRet = TS_SendData(g_ullServerInst, strEndpoint.c_str(), szMsg, uiMsgLen);
					printf("send %u data to %s, %d\n", uiMsgLen, strEndpoint.c_str(), nRet);
				}
			}
			else if (c == 'c') {
				std::lock_guard<std::mutex> lock(g_mutex4LinkList);
				int nCount = (int)g_linkList.size();
				if (nCount) {
					int nIndex = rand() % nCount;
					std::string strEndpoint = g_linkList[nIndex];
					int nRet = TS_CloseEndpoint(g_ullServerInst, strEndpoint.c_str());
					printf("close %s, %d\n", strEndpoint.c_str(), nRet);
				}
			}
		}
		TS_StopServer(g_ullServerInst);
		g_cond4ReplyQue.notify_one();
		thd_reply.join();
		g_ullServerInst = 0;
	}
	return 0;
}
