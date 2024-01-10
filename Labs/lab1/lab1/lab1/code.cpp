#include <stdio.h>
#include <Windows.h>
#include <process.h>
#include <string.h>

#pragma comment(lib,"Ws2_32.lib")
#define MAXSIZE 65507 //发送数据报⽂的最⼤⻓度
#define HTTP_PORT 80 //http 服务器端⼝

#define invalid_website "http://today.hit.edu.cn/" //屏蔽网址
#define fish_web_src "http://www.7k7k.com/" //钓鱼源网址
#define fish_web_url "http://jwts.hit.edu.cn/" //钓鱼目的网址
#define fish_web_host "jwts.hit.edu.cn" //钓鱼目的地址的主机名

//Http 重要头部数据
struct HttpHeader {
    char method[4]; // POST 或者 GET，注意有些为 CONNECT，本实验暂不考虑
    char url[1024]; // 请求的 url
    char host[1024]; // ⽬标主机
    char cookie[1024 * 10]; //cookie
    HttpHeader() {
        ZeroMemory(this, sizeof(HttpHeader));
    }
};

BOOL InitSocket();
int ParseHttpHead(char* buffer, HttpHeader* httpHeader);
BOOL ConnectToServer(SOCKET* serverSocket, char* host);
unsigned int __stdcall ProxyThread(LPVOID lpParameter);
void makeFilename(char* url, char* filename);
void getCache(char* buffer, char* filename);
void makeCache(char* buffer, char* url);
void makeNewHTTP(char* buffer, char* value);
void getDate(char* buffer, char* field, char* tempDate);

//代理相关参数
SOCKET ProxyServer;
sockaddr_in ProxyServerAddr;
const int ProxyPort = 10240;
const char* forbid_user[10] = { "127.0.0.1" };  //被屏蔽的用户IP
bool flag = true;  //用户过滤开启或关闭 

//由于新的连接都使⽤新线程进⾏处理，对线程的频繁的创建和销毁特别浪费资源
//可以使⽤线程池技术提⾼服务器效率
//const int ProxyThreadMaxNum = 20;
//HANDLE ProxyThreadHandle[ProxyThreadMaxNum] = {0};
//DWORD ProxyThreadDW[ProxyThreadMaxNum] = {0};

struct ProxyParam {
    SOCKET clientSocket;
    SOCKET serverSocket;
};

int main(int argc, char* argv[])
{
    sockaddr_in addr_in;
    int addr_len = sizeof(SOCKADDR);

    printf("代理服务器正在启动\n");
    printf("初始化...\n");
    if (!InitSocket()) {
        printf("socket 初始化失败\n");
        return -1;
    }
    printf("代理服务器正在运行，监听端口 %d\n", ProxyPort);
    SOCKET acceptSocket = INVALID_SOCKET;
    ProxyParam* lpProxyParam;
    HANDLE hThread;
    DWORD dwThreadID;
    //代理服务器不断监听
    while (true) {
        //获取用户主机ip地址
        acceptSocket = accept(ProxyServer, (SOCKADDR*)&addr_in, &(addr_len));
        lpProxyParam = new ProxyParam;
        if (lpProxyParam == NULL) {
            continue;
        }
        //用户过滤
        if (!strcmp(forbid_user[0], inet_ntoa(addr_in.sin_addr)) && flag)//inet_ntoa把网络字节序的地址转化为点分十进制的地址
        {
            printf("该用户访问受限\n");
            continue;
        }
        lpProxyParam->clientSocket = acceptSocket;
        hThread = (HANDLE)_beginthreadex(NULL, 0, &ProxyThread, (LPVOID)lpProxyParam, 0, 0);
        CloseHandle(hThread);
        Sleep(200);
    }
    closesocket(ProxyServer);
    WSACleanup();
    return 0;
}

//************************************
// Method:  InitSocket
// FullName: InitSocket
// Access:public
// Returns:BOOL
// Qualifier: 初始化套接字
//************************************
BOOL InitSocket() {
    //加载套接字库（必须）
    WORD wVersionRequested;
    WSADATA wsaData;
    //套接字加载时错误提示
    int err;
    //版本 2.2
    wVersionRequested = MAKEWORD(2, 2);
    //加载 dll ⽂件 Scoket 库
    err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        //找不到 winsock.dll
        printf("加载 winsock 失败，错误代码为: %d\n", WSAGetLastError());
        return FALSE;
    }
    if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
    {
        printf("不能找到正确的 winsock 版本\n");
        WSACleanup();
        return FALSE;
    }
    ProxyServer = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCKET == ProxyServer) {
        printf("创建套接字失败，错误代码为：%d\n", WSAGetLastError());
        return FALSE;
    }
    ProxyServerAddr.sin_family = AF_INET;
    ProxyServerAddr.sin_port = htons(ProxyPort);
    ProxyServerAddr.sin_addr.S_un.S_addr = INADDR_ANY;
    if (bind(ProxyServer, (SOCKADDR*)&ProxyServerAddr, sizeof(SOCKADDR)) == SOCKET_ERROR) {
        printf("绑定套接字失败\n");
        return FALSE;
    }
    if (listen(ProxyServer, SOMAXCONN) == SOCKET_ERROR) {
        printf("监听端⼝%d 失败", ProxyPort);
        return FALSE;
    }
    return TRUE;
}

//************************************
// Method:ProxyThread
// FullName: ProxyThread
// Access:public
// Returns:unsigned int __stdcall
// Qualifier: 线程执⾏函数
// Parameter: LPVOID lpParameter
//************************************
unsigned int __stdcall ProxyThread(LPVOID lpParameter) {
    char Buffer[MAXSIZE];   //缓存
    char* CacheBuffer;      //缓存指针
    ZeroMemory(Buffer, MAXSIZE);

    SOCKADDR_IN clientAddr;
    int length = sizeof(SOCKADDR_IN);

    char fileBuffer[MAXSIZE];
    char *filename = (char*)calloc(100, sizeof(char));
    FILE* in;

    char* field = (char*)"Date";
    char date_str[30];
    ZeroMemory(date_str, 30);

    BOOL haveCache = false;
    HttpHeader* httpHeader = new HttpHeader();

    int recvSize;
    int ret;

    //从客户端获得http报文，存入Buffer
    recvSize = recv(((ProxyParam*)lpParameter)->clientSocket, Buffer, MAXSIZE, 0);
    if (recvSize <= 0) {
        goto error;
    }

    CacheBuffer = new char[recvSize + 1];
    ZeroMemory(CacheBuffer, recvSize + 1);
    memcpy(CacheBuffer, Buffer, recvSize);  //拷贝buffer
    //connect方式不创建连接
    if (!ParseHttpHead(CacheBuffer, httpHeader)) {
        goto error;
    }
    delete CacheBuffer;

    FILE* fp;
    errno_t err;
    makeFilename(httpHeader->url, filename);
    err = fopen_s(&in, filename, "rb");
    //如果有缓存文件，插入If-Modified-Since字段
    if (in != NULL)
    {
        fread(fileBuffer, sizeof(char), MAXSIZE, in);
        fclose(in);
        getDate(fileBuffer, field, date_str);
        printf("插入If-Modified-Since: %s\n", date_str);
        makeNewHTTP(Buffer, date_str);
        haveCache = true;
    }

    if (strcmp(httpHeader->url, invalid_website) == 0)
    {
        printf("***********该网站已被屏蔽***********\n");
        goto error;
    }

    if (strcmp(httpHeader->url, fish_web_src) == 0)
    {
        printf("*******目标网址已被引导*******\n");
        memcpy(httpHeader->host, fish_web_host, strlen(fish_web_host) + 1);
        memcpy(httpHeader->url, fish_web_url, strlen(fish_web_url));
    }

    if (!ConnectToServer(&((ProxyParam*)lpParameter)->serverSocket, httpHeader->host)) {
        goto error;
    }
    printf("代理连接主机 %s 成功\n", httpHeader->host);

    //将客户端发送的 HTTP 数据报⽂直接转发给⽬标服务器
    ret = send(((ProxyParam*)lpParameter)->serverSocket, Buffer, strlen(Buffer) + 1, 0);
    //等待⽬标服务器返回数据
    recvSize = recv(((ProxyParam*)lpParameter)->serverSocket, Buffer, MAXSIZE, 0);
    if (recvSize <= 0) {
        goto error;
    }

    // 是否有缓存，没有缓存报文
    if (haveCache)
    {
        getCache(Buffer, filename);
    }
    else {
        makeCache(Buffer, httpHeader->url);  //缓存报文
    }

    //将⽬标服务器返回的数据直接转发给客户端
    ret = send(((ProxyParam*)lpParameter)->clientSocket, Buffer, sizeof(Buffer), 0);
    printf("数据已经发送给用户\n\n");

    //错误处理
error:
    printf("关闭套接字\n");
    Sleep(200);
    closesocket(((ProxyParam*)lpParameter)->clientSocket);
    closesocket(((ProxyParam*)lpParameter)->serverSocket);
    delete lpParameter;
    _endthreadex(0);
    return 0;
}

//************************************
// Method:ParseHttpHead
// FullName: ParseHttpHead
// Access:public
// Returns:void
// Qualifier: 解析 TCP 报⽂中的 HTTP 头部
// Parameter: char * buffer
// Parameter: HttpHeader * httpHeader
//************************************
int ParseHttpHead(char* buffer, HttpHeader* httpHeader) {
    char* p;
    char* ptr;
    const char* delim = "\r\n";

    p = strtok_s(buffer, delim, &ptr);//提取第⼀⾏
    if (p[0] == 'G') {//GET ⽅式
        memcpy(httpHeader->method, "GET", 3);
        memcpy(httpHeader->url, &p[4], strlen(p) - 13);
    }
    else if (p[0] == 'P') {//POST ⽅式
        memcpy(httpHeader->method, "POST", 4);
        memcpy(httpHeader->url, &p[5], strlen(p) - 14);
    }
    else {
        return false;
    }
    //打印url
    printf("url = %s\n", httpHeader->url);
    p = strtok_s(NULL, delim, &ptr);
    while (p) {
        //printf("%s\n", p);
        switch (p[0]) {
        case 'H'://Host
            memcpy(httpHeader->host, &p[6], strlen(p) - 6);
            break;
        case 'C'://Cookie
            if (strlen(p) > 8) {
                char header[8];
                ZeroMemory(header, sizeof(header));
                memcpy(header, p, 6);
                if (!strcmp(header, "Cookie")) {
                    memcpy(httpHeader->cookie, &p[8], strlen(p) - 8);
                }
            }
            break;
        default:
            break;
        }
        p = strtok_s(NULL, delim, &ptr);
    }
    return true;
}

//************************************
// Method:ConnectToServer
// FullName: ConnectToServer
// Access:public
// Returns:BOOL
// Qualifier: 根据主机创建⽬标服务器套接字，并连接
// Parameter: SOCKET * serverSocket
// Parameter: char * host
//************************************
BOOL ConnectToServer(SOCKET* serverSocket, char* host) {
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(HTTP_PORT);
    HOSTENT* hostent = gethostbyname(host);
    if (!hostent) {
        return FALSE;
    }
    in_addr Inaddr = *((in_addr*)*hostent->h_addr_list);
    serverAddr.sin_addr.s_addr = inet_addr(inet_ntoa(Inaddr));
    //printf("%s\n", inet_ntoa(Inaddr));
    *serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (*serverSocket == INVALID_SOCKET) {
        return FALSE;
    }
    if (connect(*serverSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(*serverSocket);
        return FALSE;
    }
    return TRUE;
}

//用url来为文件命名
void makeFilename(char* url, char* filename) {
    int count = 0;
    int len;
    while (*url != '\0') {
        if ((*url >= 'a' && *url <= 'z') || (*url >= 'A' && *url <= 'Z') || (*url >= '0' && *url <= '9')) {
            *filename++ = *url;
            count++;
        }
        if (count >= 95)
            break;
        url++;
    }
    len = strlen(filename) + 1;
    strcat(filename,  ".txt");
}

void getCache(char* buffer, char* filename) {
    char* p, * ptr, num[10], tempBuffer[MAXSIZE + 1];
    const char* delim = "\r\n";
    errno_t err;
    ZeroMemory(num, 10);
    ZeroMemory(tempBuffer, MAXSIZE + 1);

    //对tempbuffer进行操作
    memcpy(tempBuffer, buffer, strlen(buffer));

    p = strtok_s(tempBuffer, delim, &ptr);//提取第一行
    memcpy(num, &p[9], 3);
    //printf("%s\n", buffer);

    if (strcmp(num, "304") == 0) {  //主机返回的报文中的状态码为304时返回已缓存的内容
        ZeroMemory(buffer, strlen(buffer));
        printf("*************************************\n");
        printf("从本机获得缓存\n");
        FILE* in = NULL;
        err = fopen_s(&in, filename, "r");
        //从文件中获取缓存的内容
        if (in != NULL) {
            fread(buffer, sizeof(char), MAXSIZE, in);
            fclose(in);
        }
    }
}

//创建cache
void makeCache(char* buffer, char* url) {
    char* p, * ptr, num[10], tempBuffer[MAXSIZE + 1];
    const char* delim = "\r\n";

    ZeroMemory(num, 10);
    ZeroMemory(tempBuffer, MAXSIZE + 1);
    memcpy(tempBuffer, buffer, strlen(buffer));

    p = strtok_s(tempBuffer, delim, &ptr);//提取第一行
    memcpy(num, &p[9], 3);
    //printf("%s\n", num);

    if (strcmp(num, "200") == 0) {  //状态码是200时缓存
        // 200指成功访问，404就是没成功

        // 构建文件
        char filename[100];
        ZeroMemory(filename, 100);
        makeFilename(url, filename);
        printf("filename : %s\n", filename);

        FILE *out, *fp;
        errno_t err;
        err = fopen_s(&fp, filename, "w");
        fwrite(buffer, sizeof(char), strlen(buffer), fp);
        fclose(fp);
        printf("************************************\n");
        printf("网页已经被缓存\n");
    }
}

void makeNewHTTP(char* buffer, char* value) {
    const char* field = "Host";
    const char* newfield = "If-Modified-Since: ";
    //const char *delim = "\r\n";
    char temp[MAXSIZE];
    ZeroMemory(temp, MAXSIZE);

    char* pos = strstr(buffer, field);
    int i = 0;
    for (i = 0; i < strlen(pos); i++) {
        temp[i] = pos[i];
    }
    *pos = '\0';
    while (*newfield != '\0') {  //插入If-Modified-Since字段
        *pos++ = *newfield++;
        //printf("%c", *pos);
    }
    while (*value != '\0') {
        *pos++ = *value++;
        //printf("%c", *pos);
    }
    *pos++ = '\r';
    *pos++ = '\n';
    for (i = 0; i < strlen(temp); i++) {
        *pos++ = temp[i];
    }
    //printf("报文首部变为\n%s\n", buffer);
}

//获取当前日期date，存入tempDate里
void getDate(char* buffer, char* field, char* tempDate) {
    char* p, * ptr, temp[5];
    ZeroMemory(temp, 5);
    //*field = "If-Modified-Since";

    const char* delim = "\r\n";
    p = strtok_s(buffer, delim, &ptr); // 按行读取
    //printf("%s\n", p);
    int len = strlen(field) + 2;
    while (p) {
        if (strstr(p, field) != NULL) {
            // 如果p中包含field字串，将&p[6]copy给tempdate
            memcpy(tempDate, &p[len], strlen(p) - len);
            // printf("tempDate: %s\n", tempDate);
        }
        p = strtok_s(NULL, delim, &ptr);
    }
}