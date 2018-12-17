# NBREDIS  
a non-block redis client library  
一个基于hiredis封装的redis非阻塞异步调用请求库.

---

**nbredis**是一个基于hiredis封装的，非阻塞的异步redis读写请求库。在异步处理上不使用hiredis自带的较为复杂的异步接口，而是使用hireids基础的api接口来实现.适合于比如游戏单进程的对于redis服务器的读写请求.    
特点概述:   
- **接口精简** ：只由少数几个API接口构成.  
- **使用简单** ：不需要hiredis自带异步IO所需要的libevent,libae等异步触发库触发异步事件.  
- **依赖较少** ：该库部署于普通linux开发环境之上，除了普通应用程序锁必须之日常库以外只依赖于hiredis和slog(见安装)  
- **多个连接** ：使用该库的应用程序可以同时链接多个redis-server，代码默认上限(1024).各个链接通过int描述符进行管理及读写请求,易于操作  

_备注_:该函数库非线程安全

---
## 安装步骤
本开发库目前只依赖于hiredis和slog，下面简单介绍下其依赖库的下载与安装
### hiredis  
这是由redis开源社区对外发布的客户端API包
下载地址:(https://github.com/redis/hiredis/archive/master.zip)  
安装方法:  
unzip xx.zip  
cd xx  
make  
make install  
_默认会将头文件安装在/usr/local/include/hiredis/目录下,动态库安装于/usr/local/lib/libnbredis.so_

### slog
由开发者本人开发的一个网络&本地日志库,能满足应用程序各项基本需求  
下载地址:(https://github.com/nmsoccer/slog/archive/master.zip)  
安装方法:  
unzip xx.zip  
./install.sh  
_默认会将头文件安装在/usr/local/include/slog/目录下,动态库安装于/usr/local/lib/libslog.so_

### nbredis
下载之后调用./install.sh编译安装  
_默认会将头文件安装在/usr/local/include/nbredis/目录下,动态库安装于/usr/local/lib/libnbredis.so_    

### compile
gcc -g demo.c -lm -lslog -lhiredis -lnbredis -o non_block  
如果找不到动态库请先将/usr/local/lib加入到/etc/ld.so.conf 然后执行/sbin/ldconfig  

---

## API
#### int redis_open(char *ip , int port , int timeout , REDIS_LOG_LEVEL log_level);
_打开一个redis-descripor描述符，并链接到目标redis-server服务器_  
* ip&port: redis-server服务器所在的IP地址和端口
* timeout: 链接超时时间(秒)
* log_level: 使用该库的日志等级，如下所示:  
```
typedef enum 
{
  REDIS_LOG_DEBUG = SL_VERBOSE,
  REDIS_LOG_INFO = SL_INFO,
  REDIS_LOG_ERR = SL_ERR
  
}REDIS_LOG_LEVEL;
```
REDIS_LOG_DEBUG将打印详细信息;REDIS_LOG_INFO将打印执行中的一般信息;REDIS_LOG_ERR:只打印错误信息  
* 返回值: >=0 成功并返回对应的redis-descripor描述符; -1:失败  

* _*备注*_  
调用该函数可以打开并链接多个redis-server实例，但不能同时open相同的ip&port二元组  

### REDIS_CONN_FLAG redis_isconnect(int rd);  
_检查一个打开的描述符之链接标记_
* rd:已成功打开的redis-descripor描述符  
* 返回值，REDIS_CONN_FLAG  
```
typedef enum
{
  REDIS_CONN_FLG_NONE = 0, //nothing happen
  REDIS_CONN_FLG_CONNECTING, //connecting
  REDIS_CONN_FLG_FAIL, //connect fail
  REDIS_CONN_FLG_CONNECTED, //connected
  REDIS_CONN_FLG_CLOSED //closed by server.
}REDIS_CONN_FLAG;

```

### int redis_reconnect(int rd);
_在已打开的描述符上进行重连_
* rd:已成功打开的redis-descripor描述符  
* 返回值:==0 成功 -1 失败  
* _*备注*_  
不能在已成功链接的描述符上进行再次重连，这样会返回失败

### int redis_tick();
_在主函数loop里进行驱动的定时检查_  
***使用库的应用进程必须将该函数纳入进程的主循环当中周期调用，否则可能无法实现库函数功能***  

### int redis_exec(int rd , char *cmd , REDIS_CALLBACK callback , char *private , int private_len);
_执行一个redis命令_  
* rd:已成功打开的redis-descripor描述符  
* cmd:redis命令  
* callback:命令执行返回之后所执行的回调函数 or NULL  
* private:回调函数自身所需要携带的私有数据 or NULL  
* private_len:私有数据长度 or 0  
* 返回值:0 success -1 failed  
##### 回调函数(REDIS_CALLBACK) 
```
typedef int (*REDIS_CALLBACK)(char *private , int private_len , REDIS_CB_RESULT result , int argc , char *argv[] , int arglen[]);
```
* private&private_len:如上所述，回传给回调函数的私有数据及数据长度  
* result: 命令执行的返回结果,如下所示:  
```
typedef enum
{
  CB_RET_ERROR = -1, //error. and error detail stores in argv[0]
  CB_RET_SUCCESS = 0, //success
  CB_RET_NO_NIL //no result
}REDIS_CB_RESULT;
```
* argc:请求结果的字符串数组长度
* argv:请求结果的字符串数组
* arglen:每个请求结果的字符串长度

### int redis_close(int rd);  
_关闭已打开的描述符并释放链接_

---
## 演示程序  
我们假设已经安装好了依赖库hiredis及slog  
1. 我们建立有两个成员的数组，分别链接两个redis-server实例  
```
#include <stdio.h>
#include <stdlib.h>
#include <nbredis/redis_non_block.h>
#include <signal.h>

#define MAX_REDIS_CONNECT 2
int rd_array[MAX_REDIS_CONNECT] = {-1};
```
2. 信号处理函数，当ctrl+c关闭进程时释放链接
```
void sig_handle(int signo)
{
  int i = 0;
  if(signo != SIGTERM && signo != SIGINT)
    return ;

  printf("catch SIG\n");
  for(i=0; i<sizeof(rd_array); i++)
  {
    if(rd_array[i] >= 0)
    {
      redis_close(rd_array[i]);
    }
  }
  exit(0);
}
```
3. 创建回调函数，用于打印结果  
```
int my_callback(char *private , int private_len , REDIS_CB_RESULT result , int argc , char *argv[] , int arglen[])
{
  int i = 0;
  char buff[1024] = {0};
  memcpy(buff , private , private_len);
  printf("[%s] result:[%d] private data:[%s] private_len:[%d] argc:[%d]\n" , __FUNCTION__ , result , buff , private_len , argc);
  if(argc > 0)
  {
    for(i=0; i<argc; i++)
    {
      memset(buff , 0 , sizeof(buff));
      memcpy(buff , argv[i] , arglen[i]);
      printf("  <%d>%s<%d>\n" , i , buff , arglen[i]);
    }
  }
  return 0;
}
```
4. 因为多个链接，所以我们将这些链接放入一个函数统一管理
```
//return:0<all connected> -1<not all connected>
int check_connect()
{
  int i = 0;
  int rd = -1;
  int flag = -1;
  for(i=0; i<MAX_REDIS_CONNECT; i++)
  {
    rd = rd_array[i];
    if(rd < 0)
      continue;

    //connect fail
    flag = redis_isconnect(rd);
    if(flag==REDIS_CONN_FLG_NONE || flag==REDIS_CONN_FLG_FAIL) //if failed and reconnect until success
    {
      printf("connect failed!\n");
      redis_reconnect(rd);
      return -1;
    }

    //in connecting
    if(flag==REDIS_CONN_FLG_CONNECTING) //connecting
    {
      printf("connecting!\n");
      return -1;
    }
    //connected
    if(flag == REDIS_CONN_FLG_CONNECTED)
      continue;

  }

  return 0;
}

```
5. 进程主函数
```
int main(int argc , char **argv)
{
  int ret = -1;
  int times = 1;  
  char *ip = "localhost";
  char trig = 0;
  char flag = -1;
  int log_level = REDIS_LOG_INFO;

  signal(SIGTERM , sig_handle);
  signal(SIGINT , sig_handle);


  ret = redis_open(ip , 6698 , 5 , log_level);
  if(ret < 0)
    return -1;  
  rd_array[0] = ret;

  ret = redis_open(ip , 6379 , 6 , log_level);
  if(ret < 0)
  {
    redis_close(rd_array[0]);
    return -1;
  }
  rd_array[1] = ret; 

  //main body
  while(1)
  {
    sleep(1);
    /***activate redis logic*/
    redis_tick();

    /****connect steps*****/
    if(check_connect() < 0)
      continue;
    /****connect step end*****/


    /***redis cmd etc.*/ 
    if(trig == 0)
    {
     redis_exec(rd_array[0] , "AUTH foobared" , my_callback , "AUTH:" , 5);     
     redis_exec(rd_array[0] , "LRANGE friend:cs 0 -1" , my_callback , "LRANGE friend:cs 0 -1" , strlen("LRANGE friend:cs 0 -1"));

     redis_exec(rd_array[1] , "PING" , my_callback , "PING:" , 5);
     redis_exec(rd_array[0] , "HGET user_info name" , my_callback , "HGET user_info name" , strlen("HGET user_info name"));
     redis_exec(rd_array[1] , "SADD fruit pear" , my_callback , "SADD fruit banana" , strlen("SADD fruit banana"));//integer
     redis_exec(rd_array[0] , "PING" , NULL , NULL , 0);//no callback
     redis_exec(rd_array[1] , "GET suomei_name" , my_callback , "GET suomei_name" , strlen("GET suomei_name")); //no exist     
     redis_exec(rd_array[0] , "HGET tank" , my_callback , "HGET tank" , strlen("HGET tank")); //err
     redis_exec(rd_array[1] , "SMEMBERS fruit" , my_callback , "SMEMBERS fruit" , strlen("SMEMBERS fruit")); 
     trig = 1;
    }
    //continue
  }
  //free
  return 0;
}
```
6. 编译并执行  
gcc -g demo.c -lm -lslog -lhiredis -lnbredis -o non_block  
_如果找不到动态库请先将/usr/local/lib加入到/etc/ld.so.conf 然后执行/sbin/ldconfig_  
下面是打印结果:
```
[my_callback] result:[0] private data:[AUTH:] private_len:[5] argc:[1]
  <0>OK<2>
[my_callback] result:[0] private data:[LRANGE friend:cs 0 -1] private_len:[21] argc:[4]
  <0>cs<2>
  <1>soul<4>
  <2>suomei<6>
  <3>suomei<6>
[my_callback] result:[0] private data:[HGET user_info name] private_len:[19] argc:[1]
  <0>soul<4>
[my_callback] result:[-1] private data:[HGET tank] private_len:[9] argc:[1]
  <0>ERR wrong number of arguments for 'hget' command<48>
[my_callback] result:[0] private data:[PING:] private_len:[5] argc:[1]
  <0>PONG<4>
[my_callback] result:[0] private data:[SADD fruit banana] private_len:[17] argc:[1]
  <0>0<1>
[my_callback] result:[1] private data:[GET suomei_name] private_len:[15] argc:[0]
[my_callback] result:[0] private data:[SMEMBERS fruit] private_len:[14] argc:[3]
  <0>apple<5>
  <1>banana<6>
  <2>pear<4>
^Ccatch SIG
```
