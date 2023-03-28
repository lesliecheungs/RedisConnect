#ifndef XG_HEAD_TYPEDEF_H
#define XG_HEAD_TYPEDEF_H

// 主要为了定义好在不同环境所需要的条件
// 类型的别名
// 控制台输出文字的颜色



// 编译器版本
#ifdef _MSC_VER //_MSC_VER 定义编译器的版本。
    // 在编程过程中难免会用到一些过时，或者曾经不安全的函数，
    // 这是编译器会出现warning提示用某某新函数，如果不想使用新的函数可以使用_CRT_SECURE_NO_WARNINGS
    #ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
    #endif

#else
    #ifndef XG_MINGW
        #define XG_LINUX
    #endif
#endif

#include <time.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <stdarg.h>
#include <assert.h>

#ifdef XG_LINUX /******* linux ******/
    #include <grp.h>
	#include <pwd.h>
	#include <errno.h>
	#include <netdb.h>
	#include <fcntl.h>
	#include <signal.h>
	#include <unistd.h>
	#include <dirent.h>
	#include <termios.h>
	#include <pthread.h>
	#include <sys/time.h>
	#include <sys/wait.h>
	#include <sys/stat.h>
	#include <sys/file.h>
	#include <sys/types.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <sys/sysinfo.h>

    #ifndef MAX_PATH
	    #define MAX_PATH	256
	#endif

	#define TRUE 		1
	#define FALSE 		0
	#define Sleep(ms)	usleep((ms)*1000)

    typedef int SOCKET;

    // 输出日志字符的颜色
    typedef enum {
        eRED = 31,
        eBLUE = 34,
        eGREEN = 32,
        eWHITE = 37,
        eYELLOW = 33
    } E_CONSOLE_COLOR;

    static int getch() {
        struct termios tm;
        struct termios old;

        int ch;
        int fd = STDIN_FILENO; // 标准输入的文件号

        // tcgetattr是一个函数，用来获取终端参数，成功返回零；
        // 失败返回非零，发生失败接口将设置errno错误标识。
        if(tcgetattr(fd, &tm) < 0)
            return 0;
        
        old = tm;
        //cfmakeraw 将终端设置为原始模式,该模式下所有的输入数据以字节为单位被处理
        cfmakeraw(&tm);

        // tcsetattr函数用于设置终端的相关参数。
        // 参数fd为打开的终端文件描述符，
        // 参数optional_actions用于控制修改起作用的时间，
        // 而结构体termios_p中保存了要修改的参数。
        /*
            TCSANOW：不等数据传输完毕就立即改变属性。
            TCSADRAIN：等待所有数据传输结束才改变属性。
            TCSAFLUSH：清空输入输出缓冲区才改变属性。
        */
        if (tcsetattr(fd, TCSANOW, &tm) < 0) return 0;

        ch = fgetc(stdin);

        if (tcsetattr(fd, TCSANOW, &old) < 0) return 0;
        return ch;
    }

    inline static void SetConsoleTextColor(E_CONSOLE_COLOR color)
	{
		printf("\e[%dm", (int)(color));
	}

#else							/****** windows *****/

	#ifndef UNICODE
	    #define UNICODE
	#endif

	#ifndef _UNICODE
	    #define _UNICODE
	#endif

	#include <windows.h>
	#include <conio.h>

	#ifdef _MSC_VER
        #define strcasecmp stricmp
        #pragma warning(disable:4996)
        #pragma comment(lib, "WS2_32.lib")
	#endif
	
	#define getch				_getch
	#define snprintf			_snprintf
	#define sleep(s)			Sleep((s)*1000)
	#define localtime_r(a,b)	localtime_s(b,a)

    	typedef enum
	{
		eRED = FOREGROUND_RED,
		eBLUE = FOREGROUND_BLUE,
		eGREEN = FOREGROUND_GREEN,
		eYELLOW = FOREGROUND_RED | FOREGROUND_GREEN,
		eWHITE = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_GREEN
	} E_CONSOLE_COLOR;

	inline static void SetConsoleTextColor(E_CONSOLE_COLOR color)
	{
		static HANDLE console = NULL;

		if (console == NULL) console = GetStdHandle(STD_OUTPUT_HANDLE);

		SetConsoleTextAttribute(console, color);
	}
#endif


#ifndef XG_AIX
typedef char 				int8;
typedef long long			int64;
typedef unsigned long long	u_int64;
#endif

typedef int					int32;
typedef short				int16;

typedef unsigned int		u_int;
typedef unsigned char		u_int8;
typedef unsigned char		u_char;
typedef unsigned long		u_long;
typedef unsigned short		u_short;

typedef unsigned int		u_int32;
typedef unsigned short		u_int16;

#ifndef ARR_LEN
#define ARR_LEN(arr)	(sizeof(arr)/sizeof(arr[0]))
#endif

#define CHECK_FALSE_RETURN(FUNC) if (FUNC){} else return false

///////////////////////////////////////////////////////
#endif