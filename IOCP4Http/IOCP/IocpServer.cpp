#include "Network.h"
#include "LockGuard.h"
#include "PerIoContext.h"
#include "PerSocketContext.h"
#include "IocpServer.h"
#include <assert.h>
#include <process.h>
#include <mswsock.h>
//for struct tcp_keepalive
#include <mstcpip.h>
#include <thread>
#include <iostream>
using namespace std;

//工作线程退出标志
#define EXIT_THREAD 0
constexpr int POST_ACCEPT_CNT = 10;

IocpServer::IocpServer(short listenPort, int maxConnectionCount) :
	m_bIsShutdown(false)
	, m_hComPort(NULL)
	, m_hExitEvent(NULL)
	, m_hWriteCompletedEvent(NULL)
	, m_listenPort(listenPort)
	, m_pListenCtx(nullptr)
	, m_nWorkerCnt(0)
	, m_nConnClientCnt(0)
	, m_nMaxConnClientCnt(maxConnectionCount)
{
	//手动reset，初始状态为nonsignaled
	m_hExitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (WSA_INVALID_EVENT == m_hExitEvent)
	{
		cout << "CreateEvent failed with error: " << WSAGetLastError() << endl;
	}
	//自动reset，初始状态为signaled
	m_hWriteCompletedEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
	if (WSA_INVALID_EVENT == m_hWriteCompletedEvent)
	{
		cout << "CreateEvent failed with error: " << WSAGetLastError() << endl;
	}
	InitializeCriticalSection(&m_csClientList);
}

IocpServer::~IocpServer()
{
	stop();
	DeleteCriticalSection(&m_csClientList);
	Network::unInit();
}

bool IocpServer::start()
{
	if (!Network::init())
	{
		cout << "network initial failed" << endl;
		return false;
	}
	if (!createListenClient(m_listenPort))
	{
		return false;
	}
	if (!createIocpWorker())
	{
		return false;
	}
	if (!initAcceptIoContext())
	{
		return false;
	}
	return true;
}

bool IocpServer::stop()
{
	//同步等待所有工作线程退出
	exitIocpWorker();
	//关闭工作线程句柄
	for_each(m_hWorkerThreads.begin(), m_hWorkerThreads.end(),
		[](const HANDLE& h) { CloseHandle(h); });
	for_each(m_acceptIoCtxList.begin(), m_acceptIoCtxList.end(),
		[](AcceptIoContext* mAcceptIoCtx) {
			CancelIo((HANDLE)mAcceptIoCtx->m_acceptSocket);
			closesocket(mAcceptIoCtx->m_acceptSocket);
			mAcceptIoCtx->m_acceptSocket = INVALID_SOCKET;
			while (!HasOverlappedIoCompleted(&mAcceptIoCtx->m_Overlapped))
			{
				Sleep(1);
			}
			delete mAcceptIoCtx;
		});
	m_acceptIoCtxList.clear();
	if (m_hExitEvent)
	{
		CloseHandle(m_hExitEvent);
		m_hExitEvent = NULL;
	}
	if (m_hComPort)
	{
		CloseHandle(m_hComPort);
		m_hComPort = NULL;
	}
	if (m_pListenCtx)
	{
		closesocket(m_pListenCtx->m_socket);
		m_pListenCtx->m_socket = INVALID_SOCKET;
		delete m_pListenCtx;
		m_pListenCtx = nullptr;
	}
	removeAllClients();
	return true;
}

bool IocpServer::shutdown()
{
	m_bIsShutdown = true;

	int ret = CancelIoEx((HANDLE)m_pListenCtx->m_socket, NULL);
	if (0 == ret)
	{
		cout << "CancelIoEx failed with error: " << WSAGetLastError() << endl;
		return false;
	}
	closesocket(m_pListenCtx->m_socket);
	m_pListenCtx->m_socket = INVALID_SOCKET;

	for_each(m_acceptIoCtxList.begin(), m_acceptIoCtxList.end(),
		[](AcceptIoContext* pAcceptIoCtx)
		{
			int ret = CancelIoEx((HANDLE)pAcceptIoCtx->m_acceptSocket, 
				&pAcceptIoCtx->m_Overlapped);
			if (0 == ret)
			{
				cout << "CancelIoEx failed with error: " << WSAGetLastError() << endl;
				return;
			}
			closesocket(pAcceptIoCtx->m_acceptSocket);
			pAcceptIoCtx->m_acceptSocket = INVALID_SOCKET;

			while (!HasOverlappedIoCompleted(&pAcceptIoCtx->m_Overlapped))
				Sleep(1);

			delete pAcceptIoCtx;
		});
	m_acceptIoCtxList.clear();

	return false;
}

bool IocpServer::send(ClientContext* pConnClient, PBYTE pData, UINT len)
{
	Buffer sendBuf;
	sendBuf.write(pData, len);
	LockGuard lk(&pConnClient->m_csLock);
	if (0 == pConnClient->m_outBuf.getBufferLen())
	{
		//第一次投递，++m_nPendingIoCnt
		enterIoLoop(pConnClient);
		pConnClient->m_outBuf.copy(sendBuf);
		pConnClient->m_sendIoCtx->m_wsaBuf.buf = (PCHAR)pConnClient->m_outBuf.getBuffer();
		pConnClient->m_sendIoCtx->m_wsaBuf.len = pConnClient->m_outBuf.getBufferLen();

		PostResult result = postSend(pConnClient);
		if (PostResult::PostResultFailed == result)
		{
			CloseClient(pConnClient);
			releaseClientContext(pConnClient);
			return false;
		}
	}
	else
	{
		pConnClient->m_outBufQueue.push(sendBuf);
	}
	//int ret = WaitForSingleObject(m_hWriteCompletedEvent, INFINITE);
	//PostQueuedCompletionStatus(m_hComPort, 0, (ULONG_PTR)pConnClient,
	//	&pConnClient->m_sendIoCtx->m_overlapped);
	return true;
}

unsigned WINAPI IocpServer::IocpWorkerThread(LPVOID arg)
{
	IocpServer* pThis = static_cast<IocpServer*>(arg);
	LPOVERLAPPED    lpOverlapped = nullptr;
	ULONG_PTR       lpCompletionKey = 0;
	DWORD           dwMilliSeconds = INFINITE;
	DWORD           dwBytesTransferred;
	int             ret;

	while (WAIT_OBJECT_0 != WaitForSingleObject(pThis->m_hExitEvent, 0))
	{
		ret = GetQueuedCompletionStatus(pThis->m_hComPort, &dwBytesTransferred,
			&lpCompletionKey, &lpOverlapped, dwMilliSeconds);

		if (EXIT_THREAD == lpCompletionKey)
		{
			//退出工作线程
			cout << "EXIT_THREAD" << endl;
			break;
		}
		// shutdown状态则停止接受连接
		if (pThis->m_bIsShutdown && lpCompletionKey == (ULONG_PTR)pThis)
		{
			continue;
		}

		if (lpCompletionKey != (ULONG_PTR)pThis)
		{
			//文档说超时的时候触发，INFINITE不会触发
			//实际上curl命令行ctrl+c强制关闭连接也会触发
			if (0 == ret)
			{
				cout << "GetQueuedCompletionStatus failed with error: "
					<< WSAGetLastError() << endl;
				pThis->handleClose(lpCompletionKey);
				continue;
			}

			//对端关闭
			if (0 == dwBytesTransferred)
			{
				pThis->handleClose(lpCompletionKey);
				continue;
			}
		}

		IoContext* pIoCtx = (IoContext*)lpOverlapped;
		switch (pIoCtx->m_PostType)
		{
		case PostType::ACCEPT:
			pThis->handleAccept(lpOverlapped, dwBytesTransferred);
			break;
		case PostType::RECV:
			pThis->handleRecv(lpCompletionKey, lpOverlapped, dwBytesTransferred);
			break;
		case PostType::SEND:
			pThis->handleSend(lpCompletionKey, lpOverlapped, dwBytesTransferred);
			break;
		default:
			break;
		}
	}

	cout << "exit" << endl;
	return 0;
}

HANDLE IocpServer::associateWithCompletionPort(SOCKET s, ULONG_PTR completionKey)
{
	HANDLE hRet;
	if (NULL == completionKey)
	{
		hRet = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	}
	else
	{
		hRet = CreateIoCompletionPort((HANDLE)s, m_hComPort, completionKey, 0);
	}
	if (NULL == hRet)
	{
		cout << "failed to associate the socket with completion port" << endl;
	}
	return hRet;
}

bool IocpServer::getAcceptExPtr()
{
	DWORD dwBytes;
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	LPFN_ACCEPTEX lpfnAcceptEx = NULL;
	int ret = WSAIoctl(m_pListenCtx->m_socket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidAcceptEx, sizeof(GuidAcceptEx),
		&lpfnAcceptEx, sizeof(lpfnAcceptEx),
		&dwBytes, NULL, NULL);
	if (SOCKET_ERROR == ret)
	{
		cout << "WSAIoctl failed with error: " << WSAGetLastError();
		closesocket(m_pListenCtx->m_socket);
		return false;
	}
	m_lpfnAcceptEx = lpfnAcceptEx;
	return true;
}

bool IocpServer::getAcceptExSockaddrs()
{
	DWORD dwBytes;
	GUID GuidAddrs = WSAID_GETACCEPTEXSOCKADDRS;
	LPFN_GETACCEPTEXSOCKADDRS lpfnGetAcceptExAddr = NULL;
	int ret = WSAIoctl(m_pListenCtx->m_socket, 
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidAddrs, sizeof(GuidAddrs),
		&lpfnGetAcceptExAddr, sizeof(lpfnGetAcceptExAddr),
		&dwBytes, NULL, NULL);
	if (SOCKET_ERROR == ret)
	{
		cout << "WSAIoctl failed with error: " << WSAGetLastError();
		closesocket(m_pListenCtx->m_socket);
		return false;
	}
	m_lpfnGetAcceptExAddr = lpfnGetAcceptExAddr;
	return true;
}

bool IocpServer::setKeepAlive(ClientContext* pConnClient,
	LPOVERLAPPED lpOverlapped, int time, int interval)
{
	if (!Network::setKeepAlive(pConnClient->m_socket, true))
		return false;

	//LPWSAOVERLAPPED pOl = &pConnClient->m_recvIoCtx->m_overlapped;
	//LPWSAOVERLAPPED pOl = nullptr;
	LPWSAOVERLAPPED pOl = lpOverlapped;

	tcp_keepalive keepAlive;
	keepAlive.onoff = 1;
	keepAlive.keepalivetime = time * 1000;
	keepAlive.keepaliveinterval = interval * 1000;
	DWORD dwBytes;
	//根据msdn这里要传一个OVERLAPPED结构
	int ret = WSAIoctl(pConnClient->m_socket, SIO_KEEPALIVE_VALS,
		&keepAlive, sizeof(tcp_keepalive), NULL, 0,
		&dwBytes, pOl, NULL);
	if (SOCKET_ERROR == ret && WSA_IO_PENDING != WSAGetLastError())
	{
		cout << "WSAIoctl failed with error: " << WSAGetLastError() << endl;
		return false;
	}
	return true;
}

bool IocpServer::createListenClient(short listenPort)
{
	m_pListenCtx = new ListenContext(listenPort);
	//创建完成端口
	m_hComPort = associateWithCompletionPort(INVALID_SOCKET, NULL);
	if (NULL == m_hComPort)
	{
		return false;
	}
	//关联监听socket和完成端口，这里将this指针作为completionKey给完成端口
	if (NULL == associateWithCompletionPort(m_pListenCtx->m_socket, (ULONG_PTR)this))
	{
		return false;
	}
	if (SOCKET_ERROR == Network::bind(m_pListenCtx->m_socket, &m_pListenCtx->m_addr))
	{
		cout << "bind failed" << endl;
		return false;
	}
	if (SOCKET_ERROR == Network::listen(m_pListenCtx->m_socket))
	{
		cout << "listen failed" << endl;
		return false;
	}
	//获取acceptEx函数指针
	if (!getAcceptExPtr())
	{
		return false;
	}
	//获取GetAcceptExSockaddrs函数指针
	if (!getAcceptExSockaddrs())
	{
		return false;
	}
	return true;
}

bool IocpServer::createIocpWorker()
{
	//根据CPU核数创建IO线程
	HANDLE hWorker;
	SYSTEM_INFO sysInfo;
	GetSystemInfo(&sysInfo);
	for (DWORD i = 0; i < sysInfo.dwNumberOfProcessors; ++i)
	{
		hWorker = (HANDLE)_beginthreadex(NULL, 0, IocpWorkerThread, this, 0, NULL);
		if (NULL == hWorker)
		{
			return false;
		}
		m_hWorkerThreads.emplace_back(hWorker);
		++m_nWorkerCnt;
	}
	cout << "started iocp worker thread count: " << m_nWorkerCnt << endl;
	return true;
}

bool IocpServer::exitIocpWorker()
{
	int ret = 0;
	SetEvent(m_hExitEvent);
	for (int i = 0; i < m_nWorkerCnt; ++i)
	{
		//通知工作线程退出
		ret = PostQueuedCompletionStatus(m_hComPort, 0, EXIT_THREAD, NULL);
		if (FALSE == ret)
		{
			cout << "PostQueuedCompletionStatus failed with error: "
				<< WSAGetLastError() << endl;
		}
	}
	//这里不明白为什么会返回0，不是应该返回m_nWorkerCnt-1吗？
	ret = WaitForMultipleObjects(m_nWorkerCnt, m_hWorkerThreads.data(), TRUE, INFINITE);
	return true;
}

bool IocpServer::initAcceptIoContext()
{
	//投递accept请求
	for (int i = 0; i < POST_ACCEPT_CNT; ++i)
	{
		AcceptIoContext* pAcceptIoCtx = new AcceptIoContext(PostType::ACCEPT);
		m_acceptIoCtxList.emplace_back(pAcceptIoCtx);
		if (!postAccept(pAcceptIoCtx))
		{
			return false;
		}
	}
	return true;
}

bool IocpServer::postAccept(AcceptIoContext* pAcceptIoCtx)
{
	pAcceptIoCtx->resetBuffer();

	DWORD dwRecvByte;
	//PCHAR pBuf = pAcceptIoCtx->m_wsaBuf.buf;
	//ULONG nLen = pAcceptIoCtx->m_wsaBuf.len - ACCEPT_ADDRS_SIZE;

	LPOVERLAPPED pOverlapped = &pAcceptIoCtx->m_Overlapped;
	LPFN_ACCEPTEX lpfnAcceptEx = (LPFN_ACCEPTEX)m_lpfnAcceptEx;

	//创建用于接受连接的socket
	pAcceptIoCtx->m_acceptSocket = Network::socket();
	if (SOCKET_ERROR == pAcceptIoCtx->m_acceptSocket)
	{
		cout << "create socket failed" << endl;
		return false;
	}

	/*
	* 使用acceptEx的一个问题：
	* 如果客户端连上却没发送数据，则acceptEx不会触发完成包，则浪费服务器资源
	* 解决方法：为了防止恶意连接，accpetEx不接收用户数据，
	* 	只接收地址（没办法，接口调用必须提供缓冲区）
	*/
	static BYTE addrBuf[DOUBLE_ACCEPT_ADDRS_SIZE];
	if (FALSE == lpfnAcceptEx(m_pListenCtx->m_socket, pAcceptIoCtx->m_acceptSocket,
		addrBuf, 0, ACCEPT_ADDRS_SIZE, ACCEPT_ADDRS_SIZE,
		&dwRecvByte, pOverlapped))
	{
		if (WSA_IO_PENDING != WSAGetLastError())
		{
			cout << "acceptEx failed" << endl;
			return false;
		}
	}
	else
	{
		// Accept completed synchronously. We need to marshal 收集
		// the data received over to the worker thread ourselves...
	}
	return true;
}

PostResult IocpServer::postRecv(ClientContext* pConnClient)
{
	PostResult result = PostResult::PostResultSucc;
	RecvIoContext* pRecvIoCtx = pConnClient->m_recvIoCtx;

	pRecvIoCtx->resetBuffer();

	LockGuard lk(&pConnClient->m_csLock);
	if (INVALID_SOCKET != pConnClient->m_socket)
	{
		DWORD dwBytes;
		//设置这个标志，则没收完的数据下一次接收
		DWORD dwFlag = MSG_PARTIAL;
		int ret = WSARecv(pConnClient->m_socket, &pRecvIoCtx->m_wsaBuf, 1,
			&dwBytes, &dwFlag, &pRecvIoCtx->m_Overlapped, NULL);
		if (SOCKET_ERROR == ret && WSA_IO_PENDING != WSAGetLastError())
		{
			cout << "WSARecv failed with error: " << WSAGetLastError() << endl;
			result = PostResult::PostResultFailed;
		}
	}
	else
	{
		result = PostResult::PostResultInvalidSocket;
	}
	return result;
}

PostResult IocpServer::postSend(ClientContext* pConnClient)
{
	PostResult result = PostResult::PostResultSucc;
	IoContext* pSendIoCtx = pConnClient->m_sendIoCtx;

	LockGuard lk(&pConnClient->m_csLock);
	if (INVALID_SOCKET != pConnClient->m_socket)
	{
		DWORD dwBytesSent;
		DWORD dwFlag = MSG_PARTIAL;
		int ret = WSASend(pConnClient->m_socket, &pSendIoCtx->m_wsaBuf, 1, &dwBytesSent,
			dwFlag, &pSendIoCtx->m_Overlapped, NULL);
		if (SOCKET_ERROR == ret && WSA_IO_PENDING != WSAGetLastError())
		{
			cout << "WSASend failed with error: " << WSAGetLastError() << endl;
			result = PostResult::PostResultFailed;
		}
	}
	else
	{
		result = PostResult::PostResultInvalidSocket;
	}
	return result;
}

bool IocpServer::handleAccept(LPOVERLAPPED lpOverlapped, DWORD dwBytesTransferred)
{
	AcceptIoContext* pAcceptIoCtx = (AcceptIoContext*)lpOverlapped;
	Network::updateAcceptContext(m_pListenCtx->m_socket, pAcceptIoCtx->m_acceptSocket);
	//达到最大连接数则关闭新的socket
	if (m_nConnClientCnt >= m_nMaxConnClientCnt)
	{
		closesocket(pAcceptIoCtx->m_acceptSocket);
		pAcceptIoCtx->m_acceptSocket = INVALID_SOCKET;
		postAccept(pAcceptIoCtx);
		return true;
	}
	InterlockedIncrement(&m_nConnClientCnt);	
	//创建新的ClientContext，原来的IoContext要用来接收新的连接
	//ClientContext刚创建，在此函数不需要加锁
	ClientContext* pConnClient = allocateClientContext(pAcceptIoCtx->m_acceptSocket);
	//memcpy_s(&pConnClient->m_addr, peerAddrLen, peerAddr, peerAddrLen);
	if (NULL == associateWithCompletionPort(pConnClient->m_socket,
		(ULONG_PTR)pConnClient))
	{
		return false;
	}
	enterIoLoop(pConnClient);
	//开启心跳机制
	//setKeepAlive(pConnClient, &pAcceptIoCtx->m_overlapped);
	//pConnClient->appendToBuffer((PBYTE)pBuf, dwBytesTransferred);
	//投递一个新的accpet请求
	postAccept(pAcceptIoCtx);
	notifyNewConnection(pConnClient);
	//notifyPackageReceived(pConnClient);
	//将客户端加入连接列表
	addClient(pConnClient);
	//投递recv请求,这里invalid socket是否要关闭客户端？
	PostResult result = postRecv(pConnClient);
	if (PostResult::PostResultFailed == result
		|| PostResult::PostResultInvalidSocket == result)
	{
		CloseClient(pConnClient);
		releaseClientContext(pConnClient);
	}
	return true;
}

bool IocpServer::handleRecv(ULONG_PTR lpCompletionKey,
	LPOVERLAPPED lpOverlapped, DWORD dwBytesTransferred)
{
	ClientContext* pConnClient = (ClientContext*)lpCompletionKey;
	RecvIoContext* pRecvIoCtx = (RecvIoContext*)lpOverlapped;
	pConnClient->appendToBuffer(pRecvIoCtx->m_recvBuf, dwBytesTransferred);
	notifyPackageReceived(pConnClient);

	//投递recv请求
	PostResult result = postRecv(pConnClient);
	if (PostResult::PostResultFailed == result
		|| PostResult::PostResultInvalidSocket == result)
	{
		CloseClient(pConnClient);
		releaseClientContext(pConnClient);
	}
	return true;
}

bool IocpServer::handleSend(ULONG_PTR lpCompletionKey,
	LPOVERLAPPED lpOverlapped, DWORD dwBytesTransferred)
{
	ClientContext* pConnClient = (ClientContext*)lpCompletionKey;
	IoContext* pIoCtx = (IoContext*)lpOverlapped;
	DWORD n = -1;

	LockGuard lk(&pConnClient->m_csLock);
	pConnClient->m_outBuf.remove(dwBytesTransferred);
	if (0 == pConnClient->m_outBuf.getBufferLen())
	{
		notifyWriteCompleted();
		pConnClient->m_outBuf.clear();

		if (!pConnClient->m_outBufQueue.empty())
		{
			pConnClient->m_outBuf.copy(pConnClient->m_outBufQueue.front());
			pConnClient->m_outBufQueue.pop();
		}
		else
		{
			releaseClientContext(pConnClient);
		}
	}
	if (0 != pConnClient->m_outBuf.getBufferLen())
	{
		pIoCtx->m_wsaBuf.buf = (PCHAR)pConnClient->m_outBuf.getBuffer();
		pIoCtx->m_wsaBuf.len = pConnClient->m_outBuf.getBufferLen();

		PostResult result = postSend(pConnClient);
		if (PostResult::PostResultFailed == result)
		{
			CloseClient(pConnClient);
			releaseClientContext(pConnClient);
		}
	}
	return false;
}

bool IocpServer::handleClose(ULONG_PTR lpCompletionKey)
{
	ClientContext* pConnClient = (ClientContext*)lpCompletionKey;
	CloseClient(pConnClient);
	releaseClientContext(pConnClient);
	return true;
}

void IocpServer::enterIoLoop(ClientContext* pClientCtx)
{
	InterlockedIncrement(&pClientCtx->m_nPendingIoCnt);
}

int IocpServer::exitIoLoop(ClientContext* pClientCtx)
{
	return InterlockedDecrement(&pClientCtx->m_nPendingIoCnt);
}

void IocpServer::CloseClient(ClientContext* pConnClient)
{
	SOCKET s;
	Addr peerAddr;
	{
		LockGuard lk(&pConnClient->m_csLock);
		s = pConnClient->m_socket;
		peerAddr = pConnClient->m_addr;
		pConnClient->m_socket = INVALID_SOCKET;
	}
	if (INVALID_SOCKET != s)
	{
		notifyDisconnected(s, peerAddr);
		if (!Network::setLinger(s))
		{
			return;
		}
		int ret = CancelIoEx((HANDLE)s, NULL);
		//ERROR_NOT_FOUND : cannot find a request to cancel
		if (0 == ret && ERROR_NOT_FOUND != WSAGetLastError())
		{
			cout << "CancelIoEx failed with error: " 
				<< WSAGetLastError() << endl;
			return;
		}

		closesocket(s);
		InterlockedDecrement(&m_nConnClientCnt);
	}
}

void IocpServer::addClient(ClientContext* pConnClient)
{
	LockGuard lk(&m_csClientList);
	m_connectedClientList.emplace_back(pConnClient);
}

void IocpServer::removeClient(ClientContext* pConnClient)
{
	LockGuard lk(&m_csClientList);
	{
		auto it = std::find(m_connectedClientList.begin(),
			m_connectedClientList.end(), pConnClient);
		if (m_connectedClientList.end() != it)
		{
			m_connectedClientList.remove(pConnClient);
			while (!pConnClient->m_outBufQueue.empty())
			{
				pConnClient->m_outBufQueue.pop();
			}
			pConnClient->m_nPendingIoCnt = 0;
			m_freeClientList.emplace_back(pConnClient);
		}
	}
}

void IocpServer::removeAllClients()
{
	LockGuard lk(&m_csClientList);
	m_connectedClientList.erase(m_connectedClientList.begin(),
		m_connectedClientList.end());
}

ClientContext* IocpServer::allocateClientContext(SOCKET s)
{
	ClientContext* pClientCtx = nullptr;
	LockGuard lk(&m_csClientList);
	if (m_freeClientList.empty())
	{
		pClientCtx = new ClientContext(s);
	}
	else
	{
		pClientCtx = m_freeClientList.front();
		m_freeClientList.pop_front();
		pClientCtx->m_nPendingIoCnt = 0;
		pClientCtx->m_socket = s;
	}
	pClientCtx->reset();
	return pClientCtx;
}

void IocpServer::releaseClientContext(ClientContext* pConnClient)
{
	if (exitIoLoop(pConnClient) <= 0)
	{
		removeClient(pConnClient);
		//这里不删除，而是将ClientContext移到空闲链表
		//delete pConnClient;
	}
}

void IocpServer::echo(ClientContext* pConnClient)
{
	send(pConnClient, pConnClient->m_inBuf.getBuffer(),
		pConnClient->m_inBuf.getBufferLen());
	pConnClient->m_inBuf.remove(pConnClient->m_inBuf.getBufferLen());
}

void IocpServer::notifyNewConnection(ClientContext* pConnClient)
{
	SOCKADDR_IN sockaddr = Network::getpeername(pConnClient->m_socket);
	pConnClient->m_addr = sockaddr;
	cout << "connected client: " << pConnClient->m_addr.toString()
		<< ", fd: " << pConnClient->m_socket << endl;
}

void IocpServer::notifyDisconnected(SOCKET s, Addr addr)
{
	cout << "closed client " << addr.toString() << ", fd: " << s << endl;
}

void IocpServer::notifyPackageReceived(ClientContext* pConnClient)
{
	echo(pConnClient);
}

void IocpServer::notifyWritePackage()
{
}

void IocpServer::notifyWriteCompleted()
{
}
