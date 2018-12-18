/*
A simple non-block redis C library
In order to solve single process using polling connect to redis
Created by soullei 2018-11-23
*/
#ifndef _REDIS_NON_BLOCK_H
#define _REDIS_NON_BLOCK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <hiredis/hiredis.h>
#include <errno.h>
#include <sys/socket.h>
#include <slog/slog.h>

#ifdef __cplusplus
extern "C" {
#endif

//log level
typedef enum 
{
  REDIS_LOG_DEBUG = SL_VERBOSE,
  REDIS_LOG_INFO = SL_INFO,
  REDIS_LOG_ERR = SL_ERR
  
}REDIS_LOG_LEVEL;

//connect flag
typedef enum
{
  REDIS_CONN_FLG_NONE = 0, //nothing happen
  REDIS_CONN_FLG_CONNECTING, //connecting
  REDIS_CONN_FLG_FAIL, //connect fail
  REDIS_CONN_FLG_CONNECTED, //connected
  REDIS_CONN_FLG_CLOSED //closed by server.
}REDIS_CONN_FLAG;

//max open
#define REDIS_MAX_OPEN_NUM  1024

/************DATA STRUCT*****************/
typedef enum
{
  CB_RET_ERROR = -1, //error. and error detail stores in argv[0]
  CB_RET_SUCCESS = 0, //success
  CB_RET_NO_NIL //no result
}REDIS_CB_RESULT;
/**
*@private&private_len callback func private data and data length
*@result:redis-cmd result. refer REDIS_CB_RESULT
*@argc: number of reply data after redis-cmd
*@argv: array of each reply data's pointer
*@arglen: array of each reply data's length
*/
typedef int (*REDIS_CALLBACK)(char *private , int private_len , REDIS_CB_RESULT result , int argc , char *argv[] , int arglen[]);

/************DATA STRUCT*****************/

/************API FUNC*****************/
/**
*open and create a connection to redis-server
*@ip&port: server ip:port
*@timeout: time out of connecting(seconds)
*@log_level: refer REDIS_LOG_LEVEL(logfile:redis_non_block.log.xx)
*@RETURN: redis-descripter
* >=0 SUCCESS -1 FAILED
**/
extern int redis_open(char *ip , int port , int timeout , REDIS_LOG_LEVEL log_level);

/**
*check connect status 
*@rd: opened redis descriptor
*@RETURN:refer REDIS_CONN_FLAG
**/
extern REDIS_CONN_FLAG redis_isconnect(int rd);

/**
*reconnect to redis-server 
*@rd: opened redis descriptor
*@RETURN: 0 SUCCESS; -1 FAIL
**/
extern int redis_reconnect(int rd);

/**
*redis-tick. [Must Be Activtated in Loop of Process!]
**/
extern int redis_tick();

/**
*exe redis cmd 
*@rd: opened redis descriptor
*@cmd: redis cmd
*@callback: callback function of application if needed. if no callback sets to NULL
*@private:  arg of callback function. if no arg sets to NULL
*@private_len: length of arg 
*@RETURN: 0 SUCCESS; -1 FAIL
**/
extern int redis_exec(int rd , char *cmd , REDIS_CALLBACK callback , char *private , int private_len);

/**
*close opened redis desciptor 
*@RETURN: 0 SUCCESS; -1 FAIL
**/
extern int redis_close(int rd);

/************API FUNC*****************/

#ifdef __cplusplus
}
#endif

#endif
