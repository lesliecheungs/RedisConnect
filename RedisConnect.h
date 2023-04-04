#ifndef REDIS_CONNECT_H
#define REDIS_CONNECT_H
///////////////////////////////////////////////////////////////
#include "ResPool.h"

#ifdef XG_LINUX

#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/statfs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/syscall.h>

#define ioctlsocket ioctl // 非阻塞模式
#define INVALID_SOCKET (SOCKET)(-1)

typedef int SOCKET;

#endif

using namespace std;

// 该类提供了一种简单和方便的方法来连接到Redis服务器并执行命令。
// 它还提供了一些有用的方法来处理Redis命令和实现分布式锁。
class RedisConnect {
    typedef std::mutex Mutex;
	typedef std::lock_guard<mutex> Locker;

	friend class Command;

// 状态定义
public:
	static const int OK = 1;
	static const int FAIL = -1;
	static const int IOERR = -2;
	static const int SYSERR = -3;
	static const int NETERR = -4;
	static const int TIMEOUT = -5;
	static const int DATAERR = -6;
	static const int SYSBUSY = -7;
	static const int PARAMERR = -8;
	static const int NOTFOUND = -9;
	static const int NETCLOSE = -10;
	static const int NETDELAY = -11;
	static const int AUTHFAIL = -12;

// 设置限制条件
public:
	static int POOL_MAXLEN; // 连接池最大容量
	static int SOCKET_TIMEOUT; // 超时阈值


public:
    // 提供了创建和管理TCP套接字的功能
    class Socket {
	protected:
		SOCKET sock = INVALID_SOCKET;
	public:
		// 将IsSocketTimeout()方法设置为static类型是因为它不需要访问类的任何成员变量或方法，也不需要创建类的实例。
		static bool IsSocketTimeout() {
#ifdef XG_LINUX
			// EAGAIN和EWOULDBLOCK都表示操作被阻塞，
			// EINTR表示操作被中断
			// 如果errno的值为0，则表示没有错误发生
			return errno == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#else
			// 函数int WSAGetLastError(void); 返回值表示该线程的最后一个Windows Sockets操作失败的错误代码
			return WSAGetLastError() == WSAETIMEOUT;
#endif
		}

		// 用于关闭指定的套接字
		static void SocketClose(SOCKET sock) {
			if(IsSocketClosed(sock))
				return;
#ifdef XG_LINUX
			::close(sock);
#else
			::closesocket(sock);
#endif

		}

		// 用于检查指定套接字是否已经关闭
		static bool IsSocketClosed(SOCKET sock) {
			return sock == INVALID_SOCKET || sock < 0;
		}

		// 用于设置套接字的发送超时时间
		static bool SocketSetSendTimeout(SOCKET sock, int timeout) {
#ifdef XG_LINUX
			struct timeval tv;
			tv.tv_sec = timeout / 1000;
			tv.tv_usec = timeout % 1000 * 1000;
			return setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)(&tv), sizeof(tv)) == 0;
#else
			return setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)(&timeout), sizeof(timeout)) == 0;
#endif
		}

		// 用于设置套接字的接收超时时间
		static bool SocketSetRecvTimeout(SOCKET sock, int timeout) {
#ifdef XG_LINUX
			struct timeval tv;

			tv.tv_sec = timeout / 1000;
			tv.tv_usec = timeout % 1000 * 1000;

			return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)(&tv), sizeof(tv)) == 0;
#else
			return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)(&timeout), sizeof(timeout)) == 0;
#endif
		}

		// 用于在指定的时间内建立套接字连接
		SOCKET SocketConnectTimeout(const char* ip, int port, int timeout) {
			u_long mode = 1;
			struct sockaddr_in addr;
			SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);

			if (IsSocketClosed(sock)) return INVALID_SOCKET;

			addr.sin_family = AF_INET;
			addr.sin_port = htons(port);
			addr.sin_addr.s_addr = inet_addr(ip);

			// 对套接字进行非阻塞设置, 使用非阻塞模式可以避免连接操作被无限期阻塞，从而提高程序的响应性能。
			// 在非阻塞模式下，connect()函数会立即返回，不会阻塞等待连接操作完成。
			ioctlsocket(sock, FIONBIO, &mode); mode = 0;

			// 对套接字进行连接操作，并在连接成功后将套接字设置为阻塞模式, 可以更方便地进行数据传输和接收。
			// 在阻塞模式下，数据传输和接收操作会被阻塞，直到操作完成或超时。这样可以避免程序中频繁地进行轮询操作，从而提高程序的效率。
			if (::connect(sock, (struct sockaddr*)(&addr), sizeof(addr)) == 0) {
				ioctlsocket(sock, FIONBIO, &mode);
				return sock;
			}
#ifdef XG_LINUX
			struct epoll_event ev;
			struct epoll_event evs;
			int handle = epoll_create(1);

			memset(&ev, 0, sizeof(ev));
			
			// 表示对套接字的写操作、错误和挂起事件感兴趣。
			ev.events = EPOLLOUT | EPOLLERR | EPOLLHUP;

			// 在套接字上发生感兴趣的事件时，系统会通知epoll实例。
			epoll_ctl(handle, EPOLL_CTL_ADD, sock, &ev);

			// 等待套接字上发生感兴趣的事件，并设置超时时间为timeout。
			if (epoll_wait(handle, &evs, 1, timeout) > 0) {
				if (evs.events & EPOLLOUT)
				{
					int res = FAIL;
					socklen_t len = sizeof(res);
					
					// 该函数用于获取套接字的错误码，以判断套接字连接是否成功
					getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)(&res), &len);
					ioctlsocket(sock, FIONBIO, &mode);

					// 如果res变量的值为0，则表示连接成功
					// 在套接字连接成功后，epoll机制已经不再需要，可以释放占用的资源。
					// 当套接字连接成功后，epoll机制已经不再需要，因为程序需要对套接字进行后续的读写操作，而不是等待事件的通知。
					if (res == 0)
					{
						::close(handle);
			
						return sock;
					}
				}
			}
			
			::close(handle);
#else
			struct timeval tv;

			fd_set ws;
			FD_ZERO(&ws);
			FD_SET(sock, &ws);

			tv.tv_sec = timeout / 1000;
			tv.tv_usec = timeout % 1000 * 1000;

			if (select(sock + 1, NULL, &ws, NULL, &tv) > 0)
			{
				int res = ERROR;
				int len = sizeof(res);
			
				getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)(&res), &len);
				ioctlsocket(sock, FIONBIO, &mode);
			
				if (res == 0) return sock;
			}
#endif
			SocketClose(sock);
			
			return INVALID_SOCKET;
		}
	
	public:
		// 该函数调用SocketClose()函数关闭套接字，
		// 并将套接字变量sock的值设置为INVALID_SOCKET，表示该套接字已经关闭。
		void close() {
			SocketClose(sock);
			sock = INVALID_SOCKET;
		}

		// 判断套接字是否关闭
		bool isClosed() const {
			return IsSocketClosed(sock);
		}

		// 设置套接字的发送超时时间
		bool setSendTimeout(int timeout) {
			return SocketSetSendTimeout(sock, timeout);
		}

		// 设置套接字的接收超时时间
		bool setRecvTimeout(int timeout) {
			return SocketSetRecvTimeout(sock, timeout);
		}

		// 连接指定的IP地址和端口号，并设置连接的超时时间。
		bool connect(const string& ip, int port, int timeout) {
			close();
			sock = SocketConnectTimeout(ip.c_str(), port, timeout);

			return IsSocketClosed(sock) ? false : true;
		}

	public:
		// 向套接字写入指定的数据，返回实际写入的字节数。
		int write(const void* data, int count) {
			// / 将数据转换为字符指针
			const char* str = (const char*)(data);

			int num = 0;
			int times = 0;
			int writed = 0;

			while (writed < count) {
				// // 使用send函数进行写入操作
				if ((num = send(sock, str + writed, count - writed, 0)) > 0) {
					if (num > 8) {
						times = 0;
					}
					else {
						//  如果连续写入失败次数超过100次，则返回超时错误
						if (++times > 100) return TIMEOUT;
					}

					writed += num;
				}
				else {
					// 如果写入操作因为超时而失败，则继续进行写入操作
					if (IsSocketTimeout()) {
						if (++times > 100) return TIMEOUT;

						continue;
					}
					 // 如果写入操作因为其他错误而失败，则返回网络错误
					return NETERR;
				}
			}
			// 返回实际写入的字节数
			return writed;
		}

		// read函数的作用是从套接字读取指定长度的数据，并将读取的数据存储到指定的缓冲区中，返回实际读取的字节数。
		// 其中completed参数用于指定读取方式。
		// 如果completed为true，则使用循环的方式进行读取操作，直到读取完成或者出现错误
		// 如果completed为false，则使用一次性的方式进行读取操作。
		int read(void* data, int count, bool completed) {
			// 将缓冲区转换为字符指针
			char* str = (char*)(data);

			if (completed) {
				int num = 0;
				int times = 0;
				int readed = 0;

				while (readed < count) {
					if ((num = recv(sock, str + readed, count - readed, 0)) > 0) {
						if (num > 8) {
							times = 0;
						}
						else {
							if (++times > 100) return TIMEOUT;
						}

						readed += num;
					}
					else if (num == 0) {
						return NETCLOSE;
					}
					else {
						if (IsSocketTimeout()) {
							if (++times > 100) return TIMEOUT;

							continue;
						}

						return NETERR;
					}
				}

				return readed;
			}
			else {
				int val = recv(sock, str, count, 0);

				if (val > 0) return val;

				if (val == 0) return NETCLOSE;

				if (IsSocketTimeout()) return 0;

				return NETERR;
			}
		}
	

	};

    // 封装redis命令
    class Command {
		friend RedisConnect;

	protected:
		int status; //  Redis 命令执行的状态码
		string msg; // Redis 命令执行的输出信息
		vector<string> res; // Redis 命令执行的结果，以字符串数组的形式保存
		vector<string> vec; // Redis 命令的参数，以字符串数组的形式保存
	
	protected:
		//  解析Redis 命令的返回结果
		int parse(const char* msg, int len) {
			// 如果是 $，则说明这是一个字符串类型的返回结果，需要调用 parseNode 函数解析该节点，并返回相应的状态码。
			if (*msg == '$')
			{
				const char* end = parseNode(msg, len);

				if (end == NULL) return DATAERR;

				switch (end - msg) {
					case 0: return TIMEOUT;
					case -1: return NOTFOUND;
				}

				return OK;
			}

			const char* str = msg + 1;
			const char* end = strstr(str, "\r\n");

			if (end == NULL) return TIMEOUT;

			// 如果 Redis 命令返回结果的第一个字符为 +、- 、:, 则说明该结果为状态码、错误信息或整数值类型的返回结果。
			if (*msg == '+' || *msg == '-' || *msg == ':') {
				this->status = OK;
				this->msg = string(str, end);

				if (*msg == '+') return OK;
				if (*msg == '-') return FAIL;

				this->status = atoi(str);

				return OK;
			}

			// 说明该结果为数组类型的返回结果。
			if (*msg == '*') {
				int cnt = atoi(str);
				const char* tail = msg + len;

				vec.clear();
				str = end + 2;

				while (cnt > 0) {
					if (*str == '*') return parse(str, tail - str);

					end = parseNode(str, tail - str);

					if (end == NULL) return DATAERR;
					if (end == str) return TIMEOUT;

					str = end;
					cnt--;
				}

				return res.size();
			}

			return DATAERR;
		}

		// 将字符串值解析成一个字符串，并将该字符串保存到 res 数组中。
		const char* parseNode(const char* msg, int len) {
			const char* str = msg + 1;
			const char* end = strstr(str, "\r\n");

			if (end == NULL) return msg;

			int sz = atoi(str);

			if (sz < 0) return msg + sz;

			str = end + 2;
			end = str + sz + 2;

			if (msg + len < end) return msg;

			res.push_back(string(str, str + sz));

			return end;
		}
	
	public:
		Command():status(0) {}

		Command(const string& cmd) {
			vec.push_back(cmd);
			this->status = 0;
		}

		void add(const char* val) {
			vec.push_back(val);
		}

		void add(const string& val) {
			vec.push_back(val);
		}

		template<class DATA_TYPE> 
		void add(DATA_TYPE val) {
			add(to_string(val));
		}

		template<class DATA_TYPE, class ...ARGS> 
		void add(DATA_TYPE val, ARGS ...args) {
			add(val);
			add(args...);
		}

	public:
		string toString() {
			ostringstream out;
			out << "*" << vec.size() << "\r\n";
			for (const string& item : vec) {
				out << "$" << item.length() << "\r\n" << item << "\r\n";
			}
			return out.str();
		}

		string get(int idx) const {
			return res.at(idx);
		}

		const vector<string>& getDataList() const {
			return res;
		}

		// 通过连接 Redis 服务器并向服务器发送 Redis 命令，
		// 然后等待 Redis 服务器返回执行结果，并将结果解析成相应的数据结构。
		int getResult(RedisConnect* redis, int timeout) {
			auto doWork = [&]() {
				// 将 Redis 命令转换成字符串后保存到 msg 变量中。
				string msg = toString();
				// 获取 Redis 连接对象中的 Socket 对象
				Socket& sock = redis->sock;

				// 将 Redis 命令发送到 Redis 服务器
				if (sock.write(msg.c_str(), msg.length()) < 0) return NETERR;

				// 定义一些变量，用于读取 Redis 服务器的响应消息
				int len = 0;
				int delay = 0;
				int readed = 0;
				char* dest = redis->buffer;
				const int maxsz = redis->memsz;

				// 进入一个循环，不停地读取 Redis 服务器的响应消息
				while (readed < maxsz) {
					// 从 Socket 对象中读取响应消息
					if ((len = sock.read(dest + readed, maxsz - readed, false)) < 0) return len;

					// 如果读取到的数据长度为 0，则说明 Redis 服务器暂时没有响应消息，需要等待一段时间 
					if (len == 0) {
						delay += SOCKET_TIMEOUT;

						if (delay > timeout) return TIMEOUT;
					}
					else {
						dest[readed += len] = 0;
						 // 如果解析失败，则继续等待 Redis 服务器的响应消息
						if ((len = parse(dest, readed)) == TIMEOUT)
						{
							delay = 0;
						}
						else
						{
							return len;
						}
					}
				}

				return PARAMERR;
			};

			status = 0;
			msg.clear();

			redis->code = doWork();

			if (redis->code < 0 && msg.empty())
			{
				switch (redis->code)
				{
				case SYSERR:
					msg = "system error";
					break;
				case NETERR:
					msg = "network error";
					break;
				case DATAERR:
					msg = "protocol error";
					break;
				case TIMEOUT:
					msg = "response timeout";
					break;
				case NOTFOUND:
					msg = "element not found";
					break;
				default:
					msg = "unknown error";
					break;
				}
			}

			redis->status = status;
			redis->msg = msg;

			return redis->code;
		}
    };

protected:
    int code = 0; // 表示Redis服务器返回的错误代码。
    int port = 0; // Redis服务器的端口号。
    int memsz = 0; // Redis服务器分配的内存大小。
    int status = 0; // 表示Redis命令的执行状态。
    int timeout = 0; // Redis命令的超时时间（以毫秒为单位）。
    char* buffer = NULL; // 用于存储从Redis服务器接收的数据的缓冲区。

    string msg; // Redis服务器返回的错误消息。
    string host; // Redis服务器的主机名或IP地址。
    Socket sock; // 用于与Redis服务器建立TCP连接的Socket对象。
    string passwd; // Redis服务器的密码（如果有）。

public:
    ~RedisConnect() {
        close();
    }

public:
    int getStatus() const {
        return status;
    }

    int getErrorCode() const {
        if(sock.isClosed())
            return FAIL;
        return code < 0? code : 0;
    }

    string getErrorString() const {
        return msg;
    }

public:
	// 用于关闭与Redis服务器的连接，并释放任何内存分配。
    void close() {
        if(buffer) {
            delete[] buffer;
            buffer = NULL;
        }
        sock.close();
    }

	// 用于重新连接到Redis服务器。
    bool reconnect() {
        if(host.empty())
            return false;
        return connect(host, port, timeout, memsz) && auth(passwd) > 0;
    }


	// 该函数的作用是执行Redis命令，并返回命令的结果
    int execute(Command& cmd) {
        return cmd.getResult(this, timeout);
    }

	// 该函数的作用是执行Redis命令，并返回命令的结果。
	// 它使用可变参数模板来接受任意数量和类型的参数，使其更加灵活和通用。
    template<class DATA_TYPE, class ...ARGS>
	int execute(DATA_TYPE val, ARGS ...args) {
		Command cmd;

		cmd.add(val, args...);

		return cmd.getResult(this, timeout);
	}

	// 用于执行Redis命令并将结果存储在一个字符串向量中，并返回Redis服务器返回的错误代码。
    template<class DATA_TYPE, class ...ARGS>
	int execute(vector<string>& vec, DATA_TYPE val, ARGS ...args) {
		Command cmd;

		cmd.add(val, args...);

		cmd.getResult(this, timeout);

		if (code > 0) std::swap(vec, cmd.res);

		return code;
	}


	// 该函数首先调用close()函数关闭先前的连接（如果有的话）。
	// 在建立新连接之前，先调用close()函数是一个良好的编程实践，可以确保代码的健壮性和可靠性。
	// 然后，它使用Socket类的connect()函数尝试建立到Redis服务器的新连接。
    bool connect(const string& host, int port, int timeout = 3000, int memsz = 2 * 1024 * 1024) {
		close();

		if (sock.connect(host, port, timeout))
		{
			sock.setSendTimeout(SOCKET_TIMEOUT);
			sock.setRecvTimeout(SOCKET_TIMEOUT);

			this->host = host;
			this->port = port;
			this->memsz = memsz;
			this->timeout = timeout;
			this->buffer = new char[memsz + 1];
		}

		return buffer ? true : false;
	}

public:
    int ping() {
		return execute("ping");
	}

	int del(const string& key) {
		return execute("del", key);
	}
	
	int ttl(const string& key) {
		return execute("ttl", key) == OK ? status : code;
	}
	
	// 用于获取指定哈希表中的字段数量。
	int hlen(const string& key) {
		return execute("hlen", key) == OK ? status : code;
	}

    int auth(const string& passwd) {
		this->passwd = passwd;

		if (passwd.empty()) return OK;

		return execute("auth", passwd);
	}

	// 用于获取指定键名对应的字符串值
	int get(const string& key, string& val) {
		vector<string> vec;

		if (execute(vec, "get", key) <= 0) return code;

		val = vec[0];

		return code;
	}
	
	// 用于减少指定键名对应的数字值。
	int decr(const string& key, int val = 1) {
		return execute("decrby", key, val);
	}
	
	// 用于增加指定键名对应的数字值。
	int incr(const string& key, int val = 1) {
		return execute("incrby", key, val);
	}

	// 用于设置指定键名的过期时间。
	int expire(const string& key, int timeout) {
		return execute("expire", key, timeout);
	}

	// 用于获取所有匹配指定模式的键名。
	int keys(vector<string>& vec, const string& key) {
		return execute(vec, "keys", key);
	}

	// 用于删除哈希表中指定字段。
	int hdel(const string& key, const string& filed) {
		return execute("hdel", key, filed);
	}

	// 用于获取哈希表中指定字段的值。
	int hget(const string& key, const string& filed, string& val) {
		vector<string> vec;

		if (execute(vec, "hget", key, filed) <= 0) return code;

		val = vec[0];

		return code;
	}


	// 用于设置指定键名的值。
	int set(const string& key, const string& val, int timeout = 0) {
		return timeout > 0 ? execute("setex", key, timeout, val) : execute("set", key, val);
	}

	// 用于设置哈希表中指定字段的值。
	int hset(const string& key, const string& filed, const string& val) {
		return execute("hset", key, filed, val);
	}

// 封装有序集合
public:
	// 用于从有序集合中删除指定元素。
	int zrem(const string& key, const string& filed)
	{
		return execute("zrem", key, filed);
	}

	// 用于向有序集合中添加元素。
	int zadd(const string& key, const string& filed, int score)
	{
		return execute("zadd", key, score, filed);
	}
	
	// 用于获取有序集合中指定范围内的元素。
	int zrange(vector<string>& vec, const string& key, int start, int end, bool withscore = false)
	{
		return withscore ? execute(vec, "zrange", key, start, end, "withscores") : execute(vec, "zrange", key, start, end);
	}

public:

	// 用于执行Lua脚本。
    template<class ...ARGS>
	int eval(const string& lua) {
		vector<string> vec;

		return eval(lua, vec);
	}

	template<class ...ARGS>
	int eval(const string& lua, const string& key, ARGS ...args) {
		vector<string> vec;
	
		vec.push_back(key);
	
		return eval(lua, vec, args...);
	}

	template<class ...ARGS>
	int eval(const string& lua, const vector<string>& keys, ARGS ...args) {
		vector<string> vec;

		return eval(vec, lua, keys, args...);
	}

	template<class ...ARGS>
	int eval(vector<string>& vec, const string& lua, const vector<string>& keys, ARGS ...args) {
		int len = 0;
		Command cmd("eval");

		cmd.add(lua);
		cmd.add(len = keys.size());

		if (len-- > 0)
		{
			for (int i = 0; i < len; i++) cmd.add(keys[i]);

			cmd.add(keys.back(), args...);
		}

		cmd.getResult(this, timeout);
	
		if (code > 0) std::swap(vec, cmd.res);

		return code;
	}

	// 用于获取指定键的值。
	string get(const string& key) {
		string res;

		get(key, res);

		return res;
	}

	// 用于获取指定哈希表中指定字段的值。
	string hget(const string& key, const string& filed) {
		string res;

		hget(key, filed, res);

		return res;
	}

	// 用于获取一个唯一的锁ID。
    const char* getLockId()
	{
		// 声明了一个线程本地存储的字符数组id
		thread_local char id[0xFF] = {0};

		// 用于获取当前主机的IP地址。
		auto GetHost = [](){
			char hostname[0xFF];

			if (gethostname(hostname, sizeof(hostname)) < 0) return "unknow host";

			struct hostent* data = gethostbyname(hostname);

			return (const char*)inet_ntoa(*(struct in_addr*)(data->h_addr_list[0]));
		};

		// 如果id数组为空，则调用GetHost()函数获取当前主机的IP地址、进程ID和线程ID，
		// 并使用snprintf()函数将它们格式化为字符串。
		if (*id == 0)
		{
#ifdef XG_LINUX
			snprintf(id, sizeof(id) - 1, "%s:%ld:%ld", GetHost(), (long)getpid(), (long)syscall(SYS_gettid));
#else
			snprintf(id, sizeof(id) - 1, "%s:%ld:%ld", GetHost(), (long)GetCurrentProcessId(), (long)GetCurrentThreadId());
#endif
		}

		// 最后，返回字符串id。
		return id;
	}

	// 用于释放指定键的锁。
    bool unlock(const string& key) {
		// Lua脚本的作用是检查指定键的值是否等于当前线程的锁ID，如果相等，则删除该键，并返回1；否则，返回0。
		const char* lua = "if redis.call('get',KEYS[1])==ARGV[1] then return redis.call('del',KEYS[1]) else return 0 end";

		return eval(lua, key, getLockId()) > 0 && status == OK;
	}

	// 用于获取指定键的锁。
	bool lock(const string& key, int timeout = 30) {
		int delay = timeout * 1000;

		for (int i = 0; i < delay; i += 10) {
			if (execute("set", key, getLockId(), "nx", "ex", timeout) >= 0) 
				return true;

			Sleep(10);
		}

		return false;
	}

protected:
	// 用于从连接池中获取一个RedisConnect对象。
    virtual shared_ptr<RedisConnect> grasp() const {
		// ResPool类的构造函数接受两个参数，一个是lambda表达式，用于创建RedisConnect对象，
		// 另一个是连接池中最多保存的对象数量。
		// 在这里，使用lambda表达式创建RedisConnect对象，并将其添加到连接池中。
		static ResPool<RedisConnect> pool([&]() {
			shared_ptr<RedisConnect> redis = make_shared<RedisConnect>();

			if (redis && redis->connect(host, port, timeout, memsz)) {
				if (redis->auth(passwd)) 
                    return redis;
			}

			return redis = NULL;
		}, POOL_MAXLEN);

		// 首先从连接池中获取一个可用的对象。
		shared_ptr<RedisConnect> redis = pool.get();

		if (redis && redis->getErrorCode()) {
			pool.disable(redis);

			return grasp();
		}

		return redis;
	}

public:
	// 用于检查当前是否可以使用Redis服务。
    static bool CanUse() {
		return GetTemplate()->port > 0;
	}

	// 用于获取当前RedisConnect对象的指针。
	static RedisConnect* GetTemplate() {
		// 该函数使用静态局部变量redis来保存RedisConnect对象，然后返回该对象的指针。
		// 由于静态局部变量只会在第一次调用函数时初始化，因此该函数始终返回同一个RedisConnect对象的指针。
		static RedisConnect redis;
		return &redis;
	}

	static void SetMaxConnCount(int maxlen) {
		if (maxlen > 0) POOL_MAXLEN = maxlen;
	}
	
	// 用于获取一个共享的RedisConnect对象。
	static shared_ptr<RedisConnect> Instance() {
		return GetTemplate()->grasp();
	}
    
	// 用于设置Redis服务器的地址、端口、超时时间、内存限制和密码等参数。
	static void Setup(const string& host, int port, const string& passwd = "", int timeout = 3000, int memsz = 2 * 1024 * 1024) {
#ifdef XG_LINUX
		// 该函数首先根据操作系统类型屏蔽SIGPIPE信号，以避免在网络连接断开后向已关闭的socket发送数据导致程序崩溃。
		signal(SIGPIPE, SIG_IGN);
#else
		WSADATA data; WSAStartup(MAKEWORD(2, 2), &data);
#endif
		RedisConnect* redis = GetTemplate();

		redis->host = host;
		redis->port = port;
		redis->memsz = memsz;
		redis->passwd = passwd;
		redis->timeout = timeout;
	}
};

int RedisConnect::POOL_MAXLEN = 8;
int RedisConnect::SOCKET_TIMEOUT = 10;

#endif