#include <cstdio>
#include <stdlib.h>
#include <WinSock2.h>
#include <time.h>
#include <fstream>
#include <iostream>
#include <process.h>
#pragma comment(lib,"ws2_32.lib")
#define SERVER_PORT 12341 //接收数据的端口号
#define CLIENT_PORT 12340 //端口号
#define SERVER_IP "0.0.0.0" //IP 地址
#define CLIENT_IP "127.0.0.1"
const int SEQ_SIZE = 10;//接收端序列号个数，为 1~10
int recv_base = 0;
BOOL recv_pack[1000];

const int BUFFER_LENGTH = 1026; //缓冲区大小，（以太网中 UDP 的数据帧中包长度应小于 1480 字节）
const int SEND_WIND_SIZE = 10;//发送窗口大小为 10，GBN 中应满足 W + 1 <= N（W 为发送窗口大小，N 为序列号个数）
//本例取序列号 0...19 共 20 个
//如果将窗口大小设为 1，则为停-等协议
//由于发送数据第一个字节如果值为 0，则数据会发送失败
//因此接收端序列号为 1~20，与发送端一一对应
BOOL ack[1000];
int curSeq;//当前数据包的 seq
int curAck;//当前等待确认的 ack
int totalSeq;//收到的包的总数
int totalPacket;//需要发送的包总数
int waitCount[1000];
int send_base;

char data[1024 * 113];
char buffer[BUFFER_LENGTH]; //数据发送接收缓冲区
SOCKADDR_IN addrClient; //客户端地址
struct ProxyParam {
};
//************************************
// Method: getCurTime
// FullName: getCurTime
// Access: public
// Returns: void
// Qualifier: 获取当前系统时间，结果存入 ptime 中
// Parameter: char * ptime
//************************************
void getCurTime(char* ptime) {
	char buffer[128];
	memset(buffer, 0, sizeof(buffer));
	time_t c_time;
	struct tm* p;
	time(&c_time);
	p = localtime(&c_time);
	sprintf_s(buffer, "%d/%d/%d %d:%d:%d",
		p->tm_year + 1900,
		p->tm_mon,
		p->tm_mday,
		p->tm_hour,
		p->tm_min,
		p->tm_sec);
	strcpy_s(ptime, sizeof(buffer), buffer);
}

void sendPacket(int index, SOCKET sockServer)
{
	buffer[0] = index;
	memcpy(&buffer[1], data + 1024 * index, 1024);
	printf("send a packet with a seq of %d\n", index);
	sendto(sockServer, buffer, BUFFER_LENGTH, 0,
		(SOCKADDR*)&addrClient, sizeof(SOCKADDR));
}
//************************************
// Method: ackHandler
// FullName: ackHandler
// Access: public 
// Returns: void
// Qualifier: 收到 ack，累积确认，取数据帧的第一个字节
//由于发送数据时，第一个字节（序列号）为 0（ASCII）时发送失败，因此加一了，此处需要减一还原
// Parameter: char c
//************************************
void ackHandler(char c) {
	unsigned char index = (unsigned char)c;
	printf("Recv a ack of %d\n", index);
	ack[index] = true;
	while (ack[send_base]) {
		printf("当前窗口：%d~%d\n", send_base, (send_base + SEND_WIND_SIZE)%20);
		ack[send_base] = 0;
		send_base++;
	}
}
/****************************************************************/
/*
	-time 从服务器端获取当前时间
	-quit 退出客户端
	-testgbn [X] 测试 SR 协议实现可靠数据传输
	[X] [0,1] 模拟数据包丢失的概率
	[Y] [0,1] 模拟 ACK 丢失的概率
*/
/****************************************************************/
void printTips() {
	printf("-----------------------------------------\n");
	printf("| -time to get current time |\n");
	printf("| -quit to exit client |\n");
	printf("| -testsr [X] [Y] to test the sr |\n");
	printf("-----------------------------------------\n");
}
//************************************
// Method: lossInLossRatio
// FullName: lossInLossRatio
// Access: public 
// Returns: BOOL
// Qualifier: 根据丢失率随机生成一个数字，判断是否丢失,丢失则返回TRUE，否则返回 FALSE
// Parameter: float lossRatio [0,1]
//************************************
BOOL lossInLossRatio(float lossRatio) {
	int lossBound = (int)(lossRatio * 100);
	int r = rand() % 101;
	if (r <= lossBound) {
		return TRUE;
	}
	return FALSE;
}
void init()
{
	printf("-----------------------------------------\n");
	printf("| 1 --客户端 |\n");
	printf("| 2 --服务器 |\n");
	printf("-----------------------------------------\n");
}

void client()
{
	printf("-----------------------------------------\n");
	printf("| run client successfully |\n");
	printf("-----------------------------------------\n");
	SOCKET socketClient = socket(AF_INET, SOCK_DGRAM, 0);
	SOCKADDR_IN addrServer;
	addrServer.sin_addr.S_un.S_addr = inet_addr(CLIENT_IP);
	//addrServer.sin_addr.S_un.S_addr = inet_pton(SERVER_IP)；
	addrServer.sin_family = AF_INET;
	addrServer.sin_port = htons(SERVER_PORT);
	//接收缓冲区
	char buffer[BUFFER_LENGTH];
	char recvPaper[1024 * 120];
	ZeroMemory(recvPaper, sizeof(recvPaper));
	ZeroMemory(buffer, sizeof(buffer));
	int len = sizeof(SOCKADDR);
	//为了测试与服务器的连接，可以使用 -time 命令从服务器端获得当前时间
		//使用 -testgbn [X] [Y] 测试 GBN 其中[X]表示数据包丢失概率
		// [Y]表示 ACK 丢包概率
	//printTips();
	int ret;
	int interval = 1;//收到数据包之后返回 ack 的间隔，默认为 1 表示每个都返回 ack，0 或者负数均表示所有的都不返回 ack
	char cmd[128];
	float packetLossRatio = 0.2; //默认包丢失率 0.2
	float ackLossRatio = 0.2; //默认 ACK 丢失率 0.2
	//用时间作为随机种子，放在循环的最外面
	srand((unsigned)time(NULL));
	int recvNum = 0;
	while (true) {
		gets_s(buffer);
		ret = sscanf(buffer, "%s%f%f", &cmd, &packetLossRatio, &ackLossRatio);
		//开始 GBN 测试，使用 GBN 协议实现 UDP 可靠文件传输
		if (!strcmp(cmd, "-testsr")) {
			printf("%s\n", "Begin to test SR protocol, please don't abort the process");
			printf("The loss ratio of packet is %.2f,the loss ratio of ack is % .2f\n", packetLossRatio, ackLossRatio);
			int waitCount = 0;
			int stage = 0;
			BOOL b;
			unsigned char u_code;//状态码
			unsigned short seq;//包的序列号
			unsigned short recvSeq;//接收窗口大小为 1，已确认的序列号
			unsigned short waitSeq;//等待的序列号
			sendto(socketClient, "-testsr", strlen("-testsr") + 1, 0,
				(SOCKADDR*)&addrServer, sizeof(SOCKADDR));
			while (true)
			{
				//等待 server 回复设置 UDP 为阻塞模式
				int recvSize = recvfrom(socketClient, buffer, BUFFER_LENGTH, 0, (SOCKADDR*)&addrServer, &len);
				//服务器状态传输完成状态码 255
				if ((unsigned char)buffer[0] == 255) {
					printf("| 数据接收成功 |\n");
					printf("| 接受的数据为 |\n");
					printf("%s\n", recvPaper);
					std::ofstream fp("re_test.txt", std::ios::out);
					fp.write(recvPaper, sizeof(recvPaper));
					fp.close();
					break;
				}
				switch (stage) {
				case 0://等待握手阶段
					u_code = (unsigned char)buffer[0];
					if ((unsigned char)buffer[0] == 205)
					{
						printf("Ready for file transmission\n");
						buffer[0] = 200;
						buffer[1] = '\0';
						sendto(socketClient, buffer, 2, 0,
							(SOCKADDR*)&addrServer, sizeof(SOCKADDR));
						stage = 1;
						recvSeq = 0;
						waitSeq = 1;
					}
					break;
				case 1://等待接收数据阶段
					seq = (unsigned short)buffer[0];
					//随机法模拟包是否丢失
					b = lossInLossRatio(packetLossRatio);
					if (b) {
						printf("The packet with a seq of %d loss\n", seq);
						continue;
					}
					printf("recv a packet with a seq of %d\n", seq);
					// 接受数据包
					memcpy(&recvPaper[seq * 1024], &buffer[1], strlen(&buffer[1]));

					buffer[0] = seq;
					buffer[1] = '\0';
					recv_pack[seq] = true;
					if (recv_base == seq) {
						//printf("当前窗口：%d~%d\n", recv_base, recv_base + SEQ_SIZE);
						while (recv_pack[recv_base]) {
							printf("当前窗口：%d~%d\n", recv_base, (recv_base + SEQ_SIZE)%20);
							recv_pack[recv_base] = 0;
							recv_base++;
						}
					}
					/*else {
						//如果当前一个包都没有收到，则等待 Seq 为 1 的数据包，不是则不返回 ACK（因为并没有上一个正确的 ACK）
						if (!recvSeq) {
							continue;
						}
						buffer[0] = recvSeq;
						buffer[1] = '\0';
					}*/
					b = lossInLossRatio(ackLossRatio);
					if (b) {
						printf("The ack of %d loss\n", (unsigned char)buffer[0]);
						continue;
					}
					sendto(socketClient, buffer, 2, 0, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
					printf("send a ack of %d\n", (unsigned char)buffer[0]);
					break;
				}
				Sleep(500);
			}
		}
		sendto(socketClient, buffer, strlen(buffer) + 1, 0, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
		ret = recvfrom(socketClient, buffer, BUFFER_LENGTH, 0, (SOCKADDR*)&addrServer, &len);
		printf("%s\n", buffer);
		if (!strcmp(buffer, "Good bye!")) {
			break;
		}
		printTips();
	}
	//关闭套接字
	closesocket(socketClient);
	WSACleanup();
}
int server(int err)
{
	printf("-----------------------------------------\n");
	printf("| run server successfully |\n");
	printf("-----------------------------------------\n");
	SOCKET sockServer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	//设置套接字为非阻塞模式
	int iMode = 1; //1：非阻塞，0：阻塞
	ioctlsocket(sockServer, FIONBIO, (u_long FAR*) & iMode);//非阻塞设置
	SOCKADDR_IN addrServer; //服务器地址
	//addrServer.sin_addr.S_un.S_addr = inet_addr(SERVER_IP);
	addrServer.sin_addr.S_un.S_addr = htonl(INADDR_ANY);//两者均可
	addrServer.sin_family = AF_INET;
	addrServer.sin_port = htons(CLIENT_PORT);
	err = bind(sockServer, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
	if (err) {
		err = GetLastError();
		printf("Could not bind the port %d for socket.Error code is % d\n", CLIENT_PORT, err);
		WSACleanup();
		return -1;
	}
	int length = sizeof(SOCKADDR);
	ZeroMemory(buffer, sizeof(buffer));
	//将测试数据读入内存
	std::ifstream icin;
	icin.open("test.txt");
	ZeroMemory(data, sizeof(data));
	icin.read(data, 1024 * 113);
	icin.close();
	int tot = strlen(data);
	totalPacket = (int)ceil((double)strlen(data) / 1024);
	int recvSize;
	for (int i = 0; i < SEQ_SIZE; ++i) {
		ack[i] = TRUE;
	}
	while (true) {
		//非阻塞接收，若没有收到数据，返回值为-1
		recvSize =
			recvfrom(sockServer, buffer, BUFFER_LENGTH, 0, ((SOCKADDR*)&addrClient), &length);
		if (recvSize < 0) {
			Sleep(200);
			continue;
		}
		printf("recv from client: %s\n", buffer);
		if (strcmp(buffer, "-time") == 0) {
			getCurTime(buffer);
		}
		else if (strcmp(buffer, "-quit") == 0) {
			strcpy_s(buffer, strlen("Good bye!") + 1, "Good bye!");
		}
		else if (strcmp(buffer, "-testsr") == 0) {
			//进入 gbn 测试阶段
			//首先 server（server 处于 0 状态）向 client 发送 205 状态码（server进入 1 状态）
				//server 等待 client 回复 200 状态码，如果收到（server 进入 2 状态），则开始传输文件，否则延时等待直至超时\
				//在文件传输阶段，server 发送窗口大小设为
			ZeroMemory(buffer, sizeof(buffer));
			ZeroMemory(ack, sizeof(ack));
			int recvSize;
			int waitTime = 0;
			printf("Begain to test SR protocol,please don't abort the process\n");
			//加入了一个握手阶段
			//首先服务器向客户端发送一个 205 大小的状态码（我自己定义的）表示服务器准备好了，可以发送数据
				//客户端收到 205 之后回复一个 200 大小的状态码，表示客户端准备好了，可以接收数据了
				//服务器收到 200 状态码之后，就开始使用 GBN 发送数据了
			printf("Shake hands stage\n");
			int stage = 0;
			bool runFlag = true;
			while (runFlag) {
				switch (stage) {
				case 0://发送 205 阶段
					buffer[0] = 205;
					sendto(sockServer, buffer, strlen(buffer) + 1, 0,
						(SOCKADDR*)&addrClient, sizeof(SOCKADDR));
					Sleep(100);
					stage = 1;
					break;
				case 1://等待接收 200 阶段，没有收到则计数器+1，超时则放弃此次“连接”，等待从第一步开始
					recvSize =
						recvfrom(sockServer, buffer, BUFFER_LENGTH, 0, ((SOCKADDR*)&addrClient), &length);
					if (recvSize < 0) {
						++waitTime;
						if (waitTime > 10) {
							runFlag = false;
							printf("Timeout error\n");
							break;
						}
						Sleep(500);
						continue;
					}
					else {
						if ((unsigned char)buffer[0] == 200) {
							printf("Begin a file transfer\n");
							printf("File size is %dB, each packet is 1024B and packet total num is % d\n", strlen(data), totalPacket);
							curSeq = 0;
							curAck = 0;
							waitTime = 0;
							stage = 2;
						}
					}
					break;
				case 2://数据传输阶段
					if (curSeq <= min(send_base + SEQ_SIZE, totalPacket - 1)) {
						//发送给客户端的序列号从 0 开始
						buffer[0] = curSeq;
						sendPacket(curSeq, sockServer);
						++curSeq;
						Sleep(500);
					}
					//等待 Ack，若没有收到，则返回值为-1，计数器+1
					recvSize = recvfrom(sockServer, buffer, BUFFER_LENGTH, 0, ((SOCKADDR*)&addrClient), &length);
					if (recvSize < 0) {
						for (int i = send_base; i <= min(send_base + SEQ_SIZE, totalPacket - 1); i++)
						{
							if (!ack[i]) {
								waitCount[i]++;
								//3 次等待 ack 则超时重传
								if (waitCount[i] > 3) {
									printf("Packet%d ack Time out\n", i);
									sendPacket(i, sockServer);
									/*memcpy(&buffer[1], data + 1024 * i, 1024);
									printf("send a packet with a seq of %d\n", i);
									sendto(sockServer, buffer, BUFFER_LENGTH, 0,
										(SOCKADDR*)&addrClient, sizeof(SOCKADDR));
									*/
									waitCount[i] = 0;
								}
							}

						}
					}
					else {
						//收到 ack
						ackHandler(buffer[0]);
						if (send_base == totalPacket) {
							runFlag = false;
							printf(" | 数据传输完成 | \n");
							buffer[0] = 255;
							sendto(sockServer, buffer, BUFFER_LENGTH, 0,
								(SOCKADDR*)&addrClient, sizeof(SOCKADDR));
						}
					}
					Sleep(500);
					break;
				}
			}
		}
		sendto(sockServer, buffer, strlen(buffer) + 1, 0, (SOCKADDR*)&addrClient,
			sizeof(SOCKADDR));
		Sleep(500);
	}
	//关闭套接字，卸载库
	closesocket(sockServer);
	WSACleanup();
	return 0;
}

unsigned int __stdcall ProxyThread(LPVOID lpParameter) {
	client();
	return 0;
}

int main(int argc, char* argv[])
{
	//加载套接字库（必须）
	WORD wVersionRequested;
	WSADATA wsaData;
	//套接字加载时错误提示
	int err;
	//版本 2.2
	wVersionRequested = MAKEWORD(2, 2);
	//加载 dll 文件 Scoket 库
	err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0) {
		//找不到 winsock.dll
		printf("WSAStartup failed with error: %d\n", err);
		return 1;
	}
	if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
	{
		printf("Could not find a usable version of Winsock.dll\n");
		WSACleanup();
	}
	else {
		printf("The Winsock 2.2 dll was found okay\n");
	}
	printTips();
	ProxyParam* lpProxyParam = new ProxyParam;
	HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, &ProxyThread, (LPVOID)lpProxyParam, NULL, 0);
	if (server(err) == -1) return -1;
}
