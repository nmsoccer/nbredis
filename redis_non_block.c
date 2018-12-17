#include "redis_non_block.h"
#include <slog/slog.h>
#include <math.h>

extern int errno;

#define REDIS_ENV_STAT_EMPTY 0
#define REDIS_ENV_STATA_VALID 1

#define CB_INFO_STAT_NULL  0
#define CB_INFO_STAT_VALID 1

#define DEFAULT_CB_PRIVATE_LEN 64 //default callback private_data len
#define DEFAULT_ARG_COUNT 1024 //default arg max count

#define REDIS_LOG "redis_non_block.log"
#define REDIS_LOG_SIZE (10*1024*1024)
#define REDIS_LOG_ROTATE 5
#define REDIS_LOG_DEGREE SLD_SEC
#define REDIS_LOG_FORMAT SLF_PREFIX

struct _cb_info
{
  char stat; //0:NULL 1:valid
  REDIS_CALLBACK func;
  char private_data[DEFAULT_CB_PRIVATE_LEN]; //if private data<=DEFAULT_CB_PRIVATE_LEN
  char *private; //pointer private_data
  int private_len; //private_data len
  struct _cb_info *next; //next
};
typedef struct _cb_info CBINFO;

typedef struct
{
  char stat;
  char flag; //flag of connect.
  //redisContext *conn;
  //redisContext *run;
  redisContext *hiredis_cxt;
  long connect_end_ts; //seconds
  char ip[64];
  int port;
  int timeout;
  int cb_count;
  CBINFO *cb_head;
  CBINFO *cb_tail;
  int id;
}
REDISENV;
//static REDISENV redis_env = {1 , NULL , NULL , 0 , {0}  , 0};


typedef struct
{
  int valid_count;
  int list_len;
  REDISENV *env_list;
  int slog_d;
}REDIS_GLOBALSPACE;
REDIS_GLOBALSPACE redis_global_space = {0 , -1 , NULL , -1};

/************INNER FUNC DEC*****************/
static int _redis_open(char *ip , int port , REDIS_LOG_LEVEL log_level);
static int _redis_connect(int rd , char *ip , int port , int timeout);
static int _check_connect(REDISENV *penv);
static int _redis_reconnect();
static int _redis_tick(REDISENV *penv);
static int _redis_tick_multi(REDISENV *env_list[] , int env_count);
static int _fd_readable(int fd , int timeout);
static int _fd_writable(int fd , int timeout);
static int _handle_reply(REDISENV *pstEnv , redisReply *pstReply);
static int _tpush_cbi(REDISENV *pstEnv , CBINFO *pstCBInfo);
static CBINFO * _hpop_cbi(REDISENV *pstEnv);
static REDISENV *_rd2env(int rd , const char *caller);
static int _reset_env(REDISENV *penv);
static void _print_space();
static int _redis_disconnect(int rd);
static void _free_cb(CBINFO *pcb);
static int _put_fd_env(int fd , REDISENV *penv);
/************INNER FUNC DEC*****************/

/************API FUNC DEFINE*****************/
//close a redis descriptor of process
//return 0:success -1:failed
int redis_close(int rd)
{
  REDISENV *penv = NULL;
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  int sld = -1;
  int real_len = 0;

  sld = pspace->slog_d;
  if(pspace->slog_d < 0)
    return -1;

  if(!pspace->env_list || pspace->list_len < 0)
    return -1;

  real_len = (int)pow(2 , pspace->list_len);  
  if(rd<0 || rd>= real_len)
  {
    slog_log(sld, SL_ERR, "<%s> failed! rd:%d illegal!", rd);
    return -1;
  }

  /***Get Env*/
  penv = &pspace->env_list[rd];
  if(penv->id != rd)
  {
    slog_log(sld, SL_ERR, "<%s> failed! id not matched! id:%d rd:%d",penv->id , rd);
    return -1;
  }

  /***Close it*/
  slog_log(sld , SL_INFO , "<%s> Close %d Before" , __FUNCTION__ , rd);
  _print_space();
  
  _redis_disconnect(rd);
  _reset_env(penv);
  memset(penv , 0 , sizeof(REDISENV));
  pspace->valid_count--;
  slog_log(sld, SL_INFO, "<%s> success! rd:%d",__FUNCTION__ , rd);
  

  slog_log(sld , SL_INFO , "<%s> Close %d After" , __FUNCTION__ , rd);
  _print_space();


  /***Check List*/
  if(pspace->valid_count <= 0)
  {
    //free
    slog_log(sld, SL_INFO, "<%s> closed and list zero. free list!", __FUNCTION__);
    free(pspace->env_list);
    pspace->list_len = -1;
    pspace->env_list = NULL;
  }
  
  return 0;
}

int redis_open(char *ip , int port , int timeout , REDIS_LOG_LEVEL log_level)
{
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  int rd = -1;
  int ret = -1;
  //check count
  if(pspace->valid_count >= REDIS_MAX_OPEN_NUM)
  {
    slog_log(pspace->slog_d , SL_ERR, "<%s> failed! opened count max! opened:%d max:%d", __FUNCTION__ , 
      pspace->valid_count , REDIS_MAX_OPEN_NUM);
    return -1;
  }
  
  //open rd
  rd = _redis_open(ip , port , log_level);
  if(rd < 0)
  {
    slog_log(pspace->slog_d , SL_ERR , "<%s> failed for open %s:%d:%d!", __FUNCTION__ , ip , port , timeout);
    return -1;
  }

  //connect
  ret = _redis_connect(rd, ip, port, timeout);
  if(ret < 0)
  {
    slog_log(pspace->slog_d , SL_ERR , "<%s> failed for connect %s:%d:%d!", __FUNCTION__ , ip , port , timeout);
    redis_close(rd);
    return -1;
  }

  slog_log(pspace->slog_d , SL_INFO, "<%s> %s:%d:%d success!", __FUNCTION__ , ip , port , timeout);
  return rd;
}

int redis_reconnect(int rd)
{
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  REDISENV *penv = NULL;
  int sld = -1;

  /***Check Basic*/
  sld = pspace->slog_d;
  if(sld < 0)
    return -1;

  /***Get Env*/
  penv = _rd2env(rd, __FUNCTION__);
  if(!penv)
    return -1;

  if(penv->flag == REDIS_CONN_FLG_CONNECTED)
  {
    slog_log(pspace->slog_d , SL_ERR, "<%s> rd:%d is connected!", rd);
    return -1;
  }

  
  return _redis_reconnect(penv);
}

//Abandon.
int redis_redirect(int rd , char *ip , int port , int timeout)
{
  REDISENV *pstEnv = NULL;
  //pstEnv = &redis_env;
  int sld = -1;
  REDIS_GLOBALSPACE *pspace = &redis_global_space;

  /***Check Basic*/
  sld = pspace->slog_d;
  if(sld < 0)
    return -1;

  /***Get Env*/
  pstEnv = _rd2env(rd, __FUNCTION__);
  if(!pstEnv)
    return -1;

  /***Handle*/
  _redis_disconnect(rd);
  //memset(pstEnv , 0 , sizeof(REDISENV));
  _reset_env(pstEnv);

  strncpy(pstEnv->ip , ip , sizeof(pstEnv->ip));
  pstEnv->port = port; 
  pstEnv->timeout = timeout;

  slog_log(sld , SL_INFO , "<%s> to %s:%d will try reconnect. timeout:%d rd:%d" , __FUNCTION__ , ip , port , 
    timeout , rd);
  return 0; 
}

REDIS_CONN_FLAG redis_isconnect(int rd)
{
  REDISENV *pstEnv = NULL;
  pstEnv = NULL;

  /***Get Env*/
  pstEnv = _rd2env(rd, __FUNCTION__);
  if(!pstEnv)
    return REDIS_CONN_FLG_FAIL;

  //if(!pstEnv->conn && pstEnv->run)
  //  return 1;
  //if(pstEnv->flag == REDIS_ENV_FLG_CONNECTED)
  //  return 1;
  

  return pstEnv->flag;
}

//Activated by main_process tick or circle
int redis_tick()
{
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  int i = -1;
  int real_len = 0;
  int valid_check = 0;
  REDISENV *env_list[REDIS_MAX_OPEN_NUM] = {NULL};
  
  //empty list
  if(!pspace->env_list || pspace->list_len < 0)
    return 0;

  //each rd tick  
  real_len = (int)pow(2 , pspace->list_len);
  for(i=0; i<real_len && valid_check<pspace->valid_count; i++)
  {
    if(pspace->env_list[i].stat == REDIS_ENV_STAT_EMPTY)
      continue;

    //_redis_tick(&pspace->env_list[i]);
    env_list[valid_check] = &pspace->env_list[i];
    valid_check++;
  }

  //multiple
  _redis_tick_multi(env_list, valid_check);

  return 0;
}

int redis_exec(int rd , char *cmd , REDIS_CALLBACK callback , char *private , int private_len)
{ 
  int ret = -1;
  REDISENV *pstEnv = NULL;
  char *ptr = NULL;

  int sld = -1;
  REDIS_GLOBALSPACE *pspace = &redis_global_space;

  /***Check Basic*/
  sld = pspace->slog_d;
  if(sld < 0)
    return -1;

  /***Get Env*/
  pstEnv = _rd2env(rd, __FUNCTION__);
  if(!pstEnv)
    return -1;

  slog_log(sld, SL_DEBUG, "<%s> starts. rd:%d cmd:%s",__FUNCTION__ , rd , cmd);
  /***Env Check*/
  if(pstEnv->flag!=REDIS_CONN_FLG_CONNECTED || !pstEnv->hiredis_cxt)
  {
    slog_log(sld , SL_ERR , "<%s>:%s failed! not connected!rd:%d flag:%d" , __FUNCTION__ , cmd , rd , 
      pstEnv->flag);
    return -1;
  }

  /***Save CallBack*/
  CBINFO *pstCBInfo = (CBINFO *)calloc(1 , sizeof(CBINFO));
  if(!pstCBInfo)
  {
    slog_log(sld , SL_ERR , "<%s> failed! Alloc CBINFO FAIL! rd:%d err:%s" , __FUNCTION__ , rd , strerror(errno));
    return -1;
  }
  do
  {
    //NO CALLBACK
    if(!callback)
      break;

    pstCBInfo->stat = CB_INFO_STAT_VALID;
    pstCBInfo->func = callback;
    pstCBInfo->private_len = private_len;
    //NO PRIVATE DATA
    if(!private || private_len<=0)
    {
      slog_log(sld , SL_DEBUG , "<%s> no private stored! rd:%d" , __FUNCTION__ , rd);      
      break;
    }

    //PRIVATE LEN <= DEFAULT_CB_PRIVATE_LEN
    if(private_len <= DEFAULT_CB_PRIVATE_LEN)
    {
      slog_log(sld , SL_VERBOSE , "<%s> default private len enough! rd:%d" , __FUNCTION__ , rd);
      pstCBInfo->private = pstCBInfo->private_data;
      memcpy(pstCBInfo->private , private , private_len);      
      break;
    }

    //PRIVATE_LEN > DEFAULT_CB_PRIVATE_LEN
    slog_log(sld , SL_VERBOSE , "<%s> calloc private data! rd:%d" , __FUNCTION__ , rd);
    pstCBInfo->private = (char *)calloc(1 , private_len);
    if(!pstCBInfo->private)
    {
      slog_log(sld , SL_ERR , "<%s> failed! Alloc CBINFO PRIVATE DATA:%d FAIL! err:%s rd:%d cmd:%s" , __FUNCTION__ , 
        private_len , strerror(errno) , rd , cmd);
      return -1;
    }
    memcpy(pstCBInfo->private , private , private_len);
    break;
  }
  while(0);

  if(_tpush_cbi(pstEnv , pstCBInfo) < 0 )
    return -1;
  
  //Append Command
  ret = redisAppendCommand(pstEnv->hiredis_cxt, cmd);
  if(ret != REDIS_OK)
  {
    slog_log(sld , SL_ERR , "<%s>:%s failed! err:%s rd:%d" , __FUNCTION__ , cmd , pstEnv->hiredis_cxt->errstr, rd);
    return -1;
  }

  return 0;
}

/************INNER FUNC DEFINE*****************/
//open a redis_descriptor for process
//return >=0:success -1:failed
static int _redis_open(char *ip , int port , REDIS_LOG_LEVEL log_level)
{
  char msg[1024] = {0};
  int rd = -1;
  int slog = -1;
  int real_len = 0;
  int new_len = 0;
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  REDISENV *penv = NULL;

  SLOG_OPTION log_option;
  int i = 0;


  //Open Log(Only Once)
  if(pspace->slog_d < 0)
  {            
    //Basic Check And Init
    memset(pspace , 0 , sizeof(REDIS_GLOBALSPACE));
    pspace->slog_d = -1;
    if(log_level<REDIS_LOG_DEBUG || log_level>REDIS_LOG_ERR)
    {
      printf("<%s> log level err! log_level:%d\n" , __FUNCTION__ , log_level);
      return -1;
    }    

    //Open
    memset(&log_option , 0 , sizeof(SLOG_OPTION));
    strncpy(log_option.type_value._local.log_name , REDIS_LOG , sizeof(log_option.type_value._local.log_name));
    log_option.log_degree = REDIS_LOG_DEGREE;
    log_option.log_size = REDIS_LOG_SIZE;
    log_option.rotate = REDIS_LOG_ROTATE;
    log_option.format = REDIS_LOG_FORMAT;
    
    pspace->slog_d = slog_open(SLT_LOCAL , log_level , &log_option , msg);
    if(pspace->slog_d < 0)
    {
      printf("[%s] failed! slog_open error! msg:%s\n" , __FUNCTION__ , msg);
      return -1;
    }
    slog = pspace->slog_d;  
    slog_log(slog, SL_INFO, "<%s> slog_open success!",__FUNCTION__);
  }
  else
    slog = pspace->slog_d;

  //Empty List
  if(!pspace->env_list || pspace->list_len < 0)
  {
    slog_log(slog , SL_INFO , "<%s> try to alloc in an empty list!" , __FUNCTION__);
    penv = (REDISENV *)calloc(1 , sizeof(REDISENV));
    if(!penv)
    {
      slog_log(slog , SL_ERR , "<%s> failed! calloc error! err:%s" , __FUNCTION__ , strerror(errno));
      return -1;
    }

    rd = 0;
    //set space info
    pspace->valid_count = 1;
    pspace->list_len = 0;//(log2(1))
    pspace->env_list = penv;

    //set env info
    penv->stat = REDIS_ENV_STATA_VALID;
    penv->id = rd;

    //print
    _print_space();
    return rd;
  }

  //No-Full List
  real_len = (int)pow(2 , pspace->list_len);

  //check ip:port duplicate
  for(i=0; i<real_len; i++)
  {
    if(pspace->env_list[i].stat == REDIS_ENV_STAT_EMPTY)
      continue;

    penv = &pspace->env_list[i];
    if(strcasecmp(penv->ip , ip)==0 && penv->port==port)
    {
      slog_log(slog, SL_ERR, "<%s> failed! ip:port[%s:%d] duplicate!", __FUNCTION__ , ip , port);
      return -1;
    }
  }
  

  //search
  if(pspace->valid_count < real_len)
  {
    for(i=0; i<real_len; i++)
    {
      if(pspace->env_list[i].stat == REDIS_ENV_STAT_EMPTY)
        break;
    }
    if(i<real_len)
    {
      //set env
      penv = &pspace->env_list[i];
      penv->stat = REDIS_ENV_STATA_VALID;
      penv->id = i;

      //set space
      pspace->valid_count++;
      rd = i;

      //print
      _print_space();
      return rd;
    }
    else //should not happen
    {
      slog_log(slog, SL_FATAL, "<%s> No-FUll List alloc Failed!valid_count:%d real_len:%d list_len:%d", 
        __FUNCTION__ , pspace->valid_count , real_len , pspace->list_len);
    }
  }

  //ALLOC NEW
  slog_log(slog, SL_INFO, "<%s> try to alloc new list",__FUNCTION__);
  new_len = (int)pow(2 , pspace->list_len+1);
  penv = (REDISENV *)calloc(new_len , sizeof(REDISENV));
  if(!penv)
  {
    slog_log(slog, SL_ERR, "<%s> alloc new list failed! new_len:%d list_len:%d",__FUNCTION__ , 
      new_len , pspace->list_len);
    return -1;
  }

    //cpy old
  memcpy(penv , pspace->env_list , real_len*sizeof(REDISENV));
    //free old
  free(pspace->env_list);
    //new
  pspace->env_list = penv;
  pspace->list_len++;
    //search
  for(i=0; i<new_len; i++)
  {
    if(pspace->env_list[i].stat == REDIS_ENV_STAT_EMPTY)
      break;
  }
  if(i >= new_len)
  {
    slog_log(slog, SL_FATAL, "<%s> alloc failed! Even In the New List! old_len:%d new_len:%d " 
      "list_len:%d" , __FUNCTION__ , real_len , new_len , pspace->list_len);
    return -1;
  }
  penv = &pspace->env_list[i];
  penv->stat = REDIS_ENV_STATA_VALID;
  penv->id = i;
  pspace->valid_count++;
  
    //print
  _print_space();

    //return
  rd = i;
  return rd;  
}


static int _redis_connect(int rd , char *ip , int port , int timeout)
{
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  REDISENV *pstEnv = NULL;
  //pstEnv = &redis_env;
  int i = 0;
  int sld = -1;
  int ret = -1;

  /***Check Basic*/
  sld = pspace->slog_d;
  if(sld < 0)
    return -1;

  /***Get Env*/
  pstEnv = _rd2env(rd, __FUNCTION__);
  if(!pstEnv)
    return -1;
  
  /***Start Connect*/
  pstEnv->flag = REDIS_CONN_FLG_CONNECTING;
  pstEnv->hiredis_cxt = redisConnectNonBlock(ip , port);
  if(!pstEnv->hiredis_cxt)
  {
    slog_log(sld , SL_ERR , "<%s> failed! rd:%d ip:%s port:%d" , __FUNCTION__ , rd , ip , port);
    return -1;
  }

  //set info
  long curr_ts = time(NULL);
  pstEnv->connect_end_ts = curr_ts + timeout;

  strncpy(pstEnv->ip , ip , sizeof(pstEnv->ip));
  pstEnv->port = port; 
  pstEnv->timeout = timeout;

  slog_log(sld , SL_INFO , "<%s> to %s:%d in progessing and will expire at:%ld rd:%d" , __FUNCTION__ , ip , port , 
    pstEnv->connect_end_ts , rd);
  return 0;
}


static int _check_connect(REDISENV *penv)
{
  fd_set rfds;
  fd_set wfds;
  int ret = 0;
  long curr_ts;
  int fd = -1;
  int sld = -1;
  int opt_value = 0;
  int opt_len = 0;

  curr_ts = time(NULL);
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  REDISENV *pstEnv = penv;
  
  sld = pspace->slog_d;
  fd = pstEnv->hiredis_cxt->fd;
  

  //if time out  
  if(curr_ts >= pstEnv->connect_end_ts)  
  {
    slog_log(sld , SL_ERR , "<%s> connect timeout!rd:%d fd:%d" , __FUNCTION__ , pstEnv->id , fd);
    return -1;
  }

  //check fd stat
  FD_ZERO(&rfds);
  FD_ZERO(&wfds);
  FD_SET(fd , &rfds);
  FD_SET(fd , &wfds);

  ret = select(fd+1 , &rfds , &wfds , NULL , NULL);
  if(ret < 0)
  {
    slog_log(sld , SL_ERR , "<%s> select failed!rd:%d fd:%d , err:%s" , __FUNCTION__ , pstEnv->id , fd , strerror(errno));
    return -1;
  }

  if(ret == 0)
  {
    slog_log(sld , SL_INFO , "<%s> connect not ready! rd%d fd:%d" , __FUNCTION__ , pstEnv->id , fd);
    return 0;
  }

  //only write good!
  if(!FD_ISSET(fd , &rfds) && FD_ISSET(fd , &wfds))
  {
    slog_log(sld , SL_INFO , "<%s> connect success! rd:%d fd:%d" , __FUNCTION__ , pstEnv->id , fd);
    pstEnv->flag = REDIS_CONN_FLG_CONNECTED;
    return 0;
  }
 
  //read and write  
  if(FD_ISSET(fd , &rfds) && FD_ISSET(fd , &wfds))
  {
    //getsockopt
    slog_log(sld , SL_INFO , "<%s> sock_fd is r&w! rd:%d fd:%d" , __FUNCTION__ , pstEnv->id , fd);
    ret = getsockopt(fd , SOL_SOCKET , SO_ERROR , &opt_value , &opt_len);
    if(ret < 0)
    {
      slog_log(sld , SL_ERR , "<%s> getsockopt failed! rd:%d fd:%d err:%s" , __FUNCTION__ , pstEnv->id , fd , strerror(errno));
      return -1; 
    }   

    //connect meets an error[linux opt_value may always 0 whenever wrong]
    if(opt_value != 0)
    {
      slog_log(sld , SL_ERR , "<%s> connect meets an error!rd:%d fd:%d errno:%d" , __FUNCTION__ , pstEnv->id , fd , opt_value);
      return -1;
    }

    
    //option 1
    /*
    //try read[[server may not push valid data firstly. except port illegal]] 
    ret = read(fd , &opt_value , sizeof(opt_value));
    if(ret < 0)
    {
      slog_log(sld , SL_ERR , "<%s> connect failed!rd:%d fd:%d err:%s" , __FUNCTION__ , pstEnv->id , fd , strerror(errno));
      return -1;
    }

    if(ret == 0)
    {
      slog_log(sld , SL_ERR , "<%s> connect failed! rd:%d fd:%d peer closed!" , __FUNCTION__ , pstEnv->id , fd);
      return -1;
    }*/

    //option 2 reconnect
    ret = connect(fd , (struct sockaddr *)pstEnv->hiredis_cxt->saddr , pstEnv->hiredis_cxt->addrlen);
    if(ret < 0)
    {      
      if(ret != EISCONN) //EISCONN shows connection is established!
      {
        slog_log(sld , SL_ERR , "<%s> connect failed! rd:%d fd:%d err:%s" , __FUNCTION__ , 
          pstEnv->id , pstEnv->hiredis_cxt->fd ,strerror(errno));
        return -1;
      }
    }
    if(ret == 0) //should not happen here
    {
      slog_log(sld , SL_FATAL , "<%s> connect meets a strange problem! please reconnect it" , __FUNCTION__);
      return -1;
    }

    //connected
    slog_log(sld , SL_INFO , "<%s> connect success! rd:%d fd:%d" , __FUNCTION__ , pstEnv->id , fd);
    //pstEnv->run = pstEnv->conn;
    //pstEnv->conn = NULL;
    pstEnv->flag = REDIS_CONN_FLG_CONNECTED;
    return 0;
  }

  slog_log(sld , SL_FATAL , "<%s> connect in some trouble! rd:%d fd:%d" , __FUNCTION__ , pstEnv->id , fd);
  return -1;
}


static int _redis_reconnect(REDISENV *penv)
{
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  REDISENV *pstEnv = NULL;
  pstEnv = penv;
  int sld = pspace->slog_d;
  int ret = -1;

  pstEnv->hiredis_cxt = redisConnectNonBlock(pstEnv->ip , pstEnv->port);
  if(!pstEnv->hiredis_cxt)
  {
    slog_log(sld , SL_ERR , "%s failed! ip:%s port:%d" , __FUNCTION__ , pstEnv->ip , pstEnv->port);
    return -1;
  }

  //set info
  long curr_ts = time(NULL);
  pstEnv->connect_end_ts = curr_ts + pstEnv->timeout;
  pstEnv->flag = REDIS_CONN_FLG_CONNECTING;

  slog_log(pspace->slog_d , SL_INFO , "<%s> to %s:%d in progessing and will expire at:%ld" , __FUNCTION__ , pstEnv->ip , pstEnv->port , 
    pstEnv->connect_end_ts);
  return 0;
}


static int _redis_tick(REDISENV *penv)
{
  REDISENV *pstEnv = NULL;
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  int sld = -1;
  int len = 0;
  /***Check Basic*/
  sld = pspace->slog_d;
  if(sld < 0)
    return -1;

  /***Get Env*/
  pstEnv = penv;

  /***Handle*/
  redisReply* reply = NULL;
  int done = -1;
  int ret = -1;
  int i = 0;
  
  //printf("%s [%d]starts\n" , __FUNCTION__ , pstEnv->id);
  //is connecting
  if(pstEnv->flag == REDIS_CONN_FLG_CONNECTING)
  {
    if(_check_connect(pstEnv) < 0) //connect failed
    {
      _redis_disconnect(pstEnv->id);
      pstEnv->flag = REDIS_CONN_FLG_FAIL;
    }
    return 0;
  }  
  
  if(pstEnv->flag != REDIS_CONN_FLG_CONNECTED)
    return 0;

  //flush output buff
  while(1)
  {
    //if(_fd_writable(pstEnv->hiredis_cxt->fd , 0) == 0)
    //{
    //  break;
    //}
    
    //printf("flush output buff\n");
    ret = redisBufferWrite(pstEnv->hiredis_cxt , &done);
    if(ret != REDIS_OK)
    {
      slog_log(sld , SL_ERR , "<%s> flush output buff failed! err:%s" , __FUNCTION__ , pstEnv->hiredis_cxt->errstr);
      break;
    }
    
    if(done == 1) //outbuff empty or clear outbuff done==1
    {      
      break;
    }

    if(done==0 && errno == EAGAIN) //rediusBuffWrite write,but not all(done==0) because send buff full(errno==EAGAIN)
    {
      slog_log(sld , SL_INFO , "<%s> sendbuff full and will write again!" , __FUNCTION__);
      break;
    }
    
  }

  //check fd readable
  if(_fd_readable(pstEnv->hiredis_cxt->fd, 0) == 0)
  {
    return 0;
  }

  //read response from server
  while(1)
  {
    //check input data available
    //if(_fd_readable(pstEnv->hiredis_cxt->fd, 0) == 0)
    //    break;

    //printf("%s read sock!\n" , __FUNCTION__);
    //read
    ret = redisBufferRead(pstEnv->hiredis_cxt);
    if(ret != REDIS_OK)
    {
      slog_log(sld , SL_ERR , "<%s> read response failed! err:%s" , __FUNCTION__ , pstEnv->hiredis_cxt->errstr);
      break;
    }
    if(errno == EAGAIN) //redisBufferRead will return REDIS_OK even errno==EAGAIN
    {
      //printf("%s no more data!\n" , __FUNCTION__);
      break;
    }
    

    //try to construct a full package consistly
    for(;;)
    {
      ret = redisGetReplyFromReader(pstEnv->hiredis_cxt, (void**)&reply);
      if(ret != REDIS_OK)
      {
        slog_log(sld , SL_ERR , "<%s> get reply error! err:%s" , __FUNCTION__ , pstEnv->hiredis_cxt->errstr);        
        break;
      }

      if(!reply)
      {
        slog_log(sld , SL_DEBUG , "<%s> no full reply is recved!" , __FUNCTION__);
        break;
      }

      //handle reply
      _handle_reply(pstEnv, reply);
      
      
      //destroy reply
      freeReplyObject(reply);
      
    } //end for:get reply
        
  } //end while:reading
  
  return 0;
}


//Activated by main_process tick or circle
static int _redis_tick_multi(REDISENV *env_list[] , int env_count)
{
  REDISENV *handle_env[REDIS_MAX_OPEN_NUM] = {NULL};
  int fd_count = 0;
  REDISENV *pstEnv = NULL;
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  int sld = -1;
  fd_set rset; // only check read fd
  
  struct timeval tv = {0 , 1000}; //1ms  
  redisReply* reply = NULL;
  int done = -1;
  int ret = -1;
  int i = 0;
  int max_fd = 0;
  int ready = 0;
  int checked = 0;

  char buf[1024*16];
  int nread;
  
  /***Check Basic*/
  sld = pspace->slog_d;
  if(sld < 0)
    return -1;

  if(env_count <= 0)
    return 0;


  /***Init*/    
  FD_ZERO(&rset);
  
  /***Check Eeach Env*/
  for(i=0; i<env_count; i++)
  {
    pstEnv = env_list[i];
    //printf("%s [%d] 1st starts\n" , __FUNCTION__ , pstEnv->id);
    slog_log(sld , SL_VERBOSE , "<%s> 1st [%d] starts" , __FUNCTION__ , pstEnv->id);
    //is connecting
    if(pstEnv->flag == REDIS_CONN_FLG_CONNECTING)
    {
      if(_check_connect(pstEnv) < 0) //connect failed
      {
        _redis_disconnect(pstEnv->id);
        pstEnv->flag = REDIS_CONN_FLG_FAIL;
      }
      continue;
    }

    //check connected
    if(pstEnv->flag != REDIS_CONN_FLG_CONNECTED)
      continue;
    
    //check fd
    handle_env[fd_count] = pstEnv;
    FD_SET(pstEnv->hiredis_cxt->fd , &rset);    
    fd_count++;

    //max fd
    if(pstEnv->hiredis_cxt->fd > max_fd)
      max_fd = pstEnv->hiredis_cxt->fd;

    //flush output buff
    while(1)
    {
      //printf("flush output buff\n");
      ret = redisBufferWrite(pstEnv->hiredis_cxt , &done);
      if(ret != REDIS_OK)
      {
        slog_log(sld , SL_ERR , "<%s> flush output buff failed! rd:%d fd:%d err:%s" , __FUNCTION__ , 
          pstEnv->id , pstEnv->hiredis_cxt->fd , pstEnv->hiredis_cxt->errstr);
        break;
      }
      
      if(done == 1) //outbuff empty or clear outbuff done==1
      {
        //printf("buffer empty!\n");
        slog_log(sld , SL_VERBOSE , "<%s> output buffer empty! rd:%d" , __FUNCTION__ , pstEnv->id);
        break;
      }
  
      if(done==0 && errno == EAGAIN) //rediusBuffWrite write,but not all(done==0) because of send buff full(errno==EAGAIN)
      {
        slog_log(sld , SL_INFO , "<%s> sendbuff full and will write again! rd:%d" , __FUNCTION__ , 
          pstEnv->id);
        break;
      }      
    }  
  }

  /***check valid fd*/
  if(fd_count <= 0)
    return 0;
    
  /***Select All Connected FD*/  
  ready = select(max_fd+1 , &rset , NULL , NULL , &tv);
  slog_log(sld, SL_VERBOSE, "<%s> 2nd select return:%d" , __FUNCTION__ , ready);
  if(ready < 0)
  {
    slog_log(sld, SL_ERR, "<%s> 2nd select failed! err:%s", __FUNCTION__ , strerror(errno));
    return -1;
  }

  /***For Each FD*/
  for(i=0; i<fd_count && checked<ready; i++)
  {
    pstEnv = handle_env[i];
    if(!FD_ISSET(pstEnv->hiredis_cxt->fd , &rset))
      continue;

    checked++;    
    //read response from server
    while(1)
    {
    
      //printf("%s read sock:%d!\n" , __FUNCTION__ , pstEnv->hiredis_cxt->fd);
      slog_log(sld , SL_VERBOSE , "%s read rd:%d sock:%d!" , __FUNCTION__ , pstEnv->id , pstEnv->hiredis_cxt->fd);

      //read fd[mainly from redisBufferRead]
      nread = read(pstEnv->hiredis_cxt->fd,buf,sizeof(buf));
      if(nread == -1) //
      {
        if(errno==EAGAIN) //no more data
        {
          slog_log(sld , SL_VERBOSE , "<%s> read no more data! rd:%d msg:%s" , __FUNCTION__ , pstEnv->id , strerror(errno));
        }
        else
        {
          slog_log(sld , SL_VERBOSE , "<%s> read failed! rd:%d msg:%s" , __FUNCTION__ , pstEnv->id , strerror(errno));  
        }
        break;
      }
      else if(nread == 0) //server closed. 
      {
        slog_log(sld, SL_INFO, "<%s> server shutdown connection! rd:%d", __FUNCTION__ , pstEnv->id);
        _redis_disconnect(pstEnv->id);
        pstEnv->flag = REDIS_CONN_FLG_CLOSED;
        break;
      }
      else //read some data
      {
        if (redisReaderFeed(pstEnv->hiredis_cxt->reader,buf,nread) != REDIS_OK) 
        {
            slog_log(sld , SL_ERR , "%s call redisReaderFeed failed! rd:%d fd:%d" , __FUNCTION__ , pstEnv->id , 
              pstEnv->hiredis_cxt->fd);
            break;
        }
      }
      
      /* old code
      //read
      ret = redisBufferRead(pstEnv->hiredis_cxt);
      if(ret != REDIS_OK)
      {
        slog_log(sld , SL_ERR , "<%s> read response failed! rd:%d fd:%d err:%s" , __FUNCTION__ , 
          pstEnv->id , pstEnv->hiredis_cxt->fd , pstEnv->hiredis_cxt->errstr);
        break;
      }
      if(errno == EAGAIN) //redisBufferRead will return REDIS_OK even errno==EAGAIN
      {
        printf("%s no more data!\n" , __FUNCTION__);
        slog_log(sld , SL_VERBOSE , "%s no more data! rd:%d fd:%d" , __FUNCTION__ , pstEnv->id , 
          pstEnv->hiredis_cxt->fd);
        break;
      }*/
      
  
      //try to construct a full package consistly
      for(;;)
      {
        ret = redisGetReplyFromReader(pstEnv->hiredis_cxt, (void**)&reply);
        if(ret != REDIS_OK)
        {
          slog_log(sld , SL_ERR , "<%s> get reply error! rd:%d fd:%d err:%s" , __FUNCTION__ , 
            pstEnv->id , pstEnv->hiredis_cxt->fd , pstEnv->hiredis_cxt->errstr);        
          break;
        }
  
        if(!reply)
        {
          slog_log(sld , SL_DEBUG , "<%s> no full reply is recved! rd:%d" , __FUNCTION__ , 
            pstEnv->id);
          break;
        }

        slog_log(sld , SL_VERBOSE , "<%s> full reply is recved! rd:%d" , __FUNCTION__ , 
            pstEnv->id);
        //handle reply
        _handle_reply(pstEnv, reply);
                
        //destroy reply
        freeReplyObject(reply);
        
      } //end for:get reply
          
    } //end while:reading
    
  }
    
  return 0;
}




static int _handle_reply(REDISENV *pstEnv , redisReply *pstReply)
{
  REDIS_GLOBALSPACE *pspace = &redis_global_space;

  int result = CB_RET_SUCCESS;
  int argc = 0;
  char *_argv[DEFAULT_ARG_COUNT] = {0};
  char **argv = _argv;
  
  int _arglen[DEFAULT_ARG_COUNT] = {0};
  int *arglen = _arglen;

  char buff[128] = {0};
  CBINFO *pstCBInfo = NULL;
  int i = 0;
  char alloc = 0; //if use calloc function
  int sld = pspace->slog_d;
  
  /***Arg Check*/
  if(!pstEnv || !pstReply)
    return -1;

  /***POP CBFUNC*/
  pstCBInfo = _hpop_cbi(pstEnv);
  if(!pstCBInfo)
  {
    slog_log(sld , SL_ERR , "<%s>:drop response for cb null! rd:%d fd:%d" , __FUNCTION__ , pstEnv->id , 
      pstEnv->hiredis_cxt->fd);
    return -1;
  }
  
  /***Construct Args*/
  slog_log(sld , SL_VERBOSE , "<%s> reply type:%d rd:%d" , __FUNCTION__ , pstReply->type , pstEnv->id);
  switch(pstReply->type)
  {
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_STATUS:    
      slog_log(sld , SL_VERBOSE , "result:%s" , pstReply->str);
      argv[argc] = pstReply->str;
      arglen[argc] = pstReply->len;
      argc++;
    break;

    case REDIS_REPLY_INTEGER:
      slog_log(sld , SL_VERBOSE , "result:%d" , pstReply->integer);
      snprintf(buff , sizeof(buff) , "%lld" , pstReply->integer);
      argv[argc] = buff;
      arglen[argc] = strlen(buff);
      argc++;
    break;

    case REDIS_REPLY_ERROR:    
      slog_log(sld , SL_VERBOSE , "result:%s" , pstReply->str);
      result = CB_RET_ERROR;
      argv[argc] = pstReply->str; //error info
      arglen[argc] = pstReply->len;
      argc++;
    break;

    case REDIS_REPLY_NIL:
      slog_log(sld , SL_VERBOSE , "result empty");
      result = CB_RET_NO_NIL;
    break;
 
    case REDIS_REPLY_ARRAY:
      //check if need alloc mem for argv and arglen
      if(pstReply->elements > DEFAULT_ARG_COUNT) 
      {
        argv = NULL;
        arglen = NULL;

        alloc = 1;
        argv = (char **)calloc(pstReply->elements , sizeof(char *));
        if(!argv)
        {
          slog_log(sld , SL_ERR , "<%s> calloc argv failed! rd:%d fd:%d err:%s" , __FUNCTION__ , pstEnv->id , 
            pstEnv->hiredis_cxt->fd , strerror(errno));
          goto _destroy;
        }

        arglen = (int *)calloc(pstReply->elements , sizeof(int));
        if(!argv)
        {
          slog_log(sld , SL_ERR , "<%s> calloc arglen failed! rd:%d fd:%d err:%s" , __FUNCTION__ , pstEnv->id , 
            pstEnv->hiredis_cxt->fd , strerror(errno));
          goto _destroy;
        }
      }

      //construct argc , argv and arglen
      for(i=0; i<pstReply->elements; i++)
      {
        slog_log(sld , SL_VERBOSE , "[%d] %s" , i , pstReply->element[i]->str);
        argv[argc] = pstReply->element[i]->str;
        arglen[argc] = pstReply->element[i]->len;
        argc++;
      }
    break;
    default:
      snprintf(buff , sizeof(buff) , "illegal reply type:%d" , pstReply->type);
      result = CB_RET_ERROR;
      argv[argc] = buff; //error info
      arglen[argc] = strlen(buff);
      argc++;
      slog_log(sld , SL_FATAL , "<%s> can not handle it right now! rd:%d fd:%d" , __FUNCTION__ , pstEnv->id , 
        pstEnv->hiredis_cxt->fd);
    break;
  }
  
  /***Call CallBack Function*/
  if(pstCBInfo->stat == CB_INFO_STAT_VALID)
  {
    slog_log(sld , SL_VERBOSE , "exe call back! rd:%d" , pstEnv->id);
    (*pstCBInfo->func)(pstCBInfo->private , pstCBInfo->private_len , result , argc , argv , arglen);
  }
  else
    slog_log(sld , SL_VERBOSE , "no call back! rd:%d" , pstEnv->id);

  /***Destroy ALLOCTED MEM*/
_destroy:
  if(alloc)
  {
    if(argv)
    {      
      free(argv);
    }
    
    if(arglen)
    {      
      free(arglen);
    }
  }

  _free_cb(pstCBInfo);
  return 0;  
}



//timeout:ms
static int _fd_readable(int fd , int timeout)
{
  struct timeval tv;
  fd_set fds;
  int ret;

  //set tv
  tv.tv_sec = timeout / 1000;
  tv.tv_usec = timeout % 1000 * 1000;

  //set fd 
  FD_ZERO(&fds);
  FD_SET(fd , &fds);

  //select
  ret = select(fd+1 , &fds , NULL , NULL , &tv);
  if(ret < 0)
  {
    printf("%s failed! select error! err:%s\n" , __FUNCTION__ , strerror(errno));
    return -1;
  }

  if(ret==1 && FD_ISSET(fd , &fds))
  {
    printf("%s ok!\n" , __FUNCTION__);
    return 1;
  }

  printf("%s not ready!\n" , __FUNCTION__);
  return 0;
}

//timeout:ms
static int _fd_writable(int fd , int timeout)
{
  struct timeval tv;
  fd_set fds;
  int ret;

  //set tv
  tv.tv_sec = timeout / 1000;
  tv.tv_usec = timeout % 1000 * 1000;

  //set fd 
  FD_ZERO(&fds);
  FD_SET(fd , &fds);

  //select
  ret = select(fd+1 , NULL , &fds , NULL , &tv);
  if(ret < 0)
  {
    printf("%s failed! select error! err:%s\n" , __FUNCTION__ , strerror(errno));
    return -1;
  }

  if(ret==1 && FD_ISSET(fd , &fds))
  {
    //printf("%s ok!\n" , __FUNCTION__);
    return 1;
  }

  printf("%s not ready!\n" , __FUNCTION__);
  return 0;
}

//PUSH a CBINFO into Tail of CallBack Chain List in Env
//return 0:success -1:failed
static int _tpush_cbi(REDISENV *pstEnv , CBINFO *pstCBInfo)
{
  /***Arg Check*/
  if(!pstEnv || !pstCBInfo)
  {
    printf("%s failed! arg null!\n" , __FUNCTION__);
    return -1;
  }

  //push an empty list
  if(pstEnv->cb_count == 0)
  {
    pstEnv->cb_head = pstCBInfo;
    pstEnv->cb_tail = pstCBInfo;
    pstEnv->cb_count++;
    return 0;
  }

  //push to tail
  pstEnv->cb_tail->next = pstCBInfo;
  pstEnv->cb_tail = pstCBInfo;
  pstEnv->cb_count++;
  return 0;
}

//POP a CBINFO out of head of CallBack Chain List in Env
//return NULL:failed or empy else pointer of CBINFO
static CBINFO * _hpop_cbi(REDISENV *pstEnv)
{
  CBINFO *pstCBInfo = NULL;
  
  /***Arg Check*/
  if(!pstEnv)
  {
    printf("%s failed! arg null!\n" , __FUNCTION__);
    return NULL;
  }
  
  if(pstEnv->cb_count <= 0)
  {
    printf("%s failed! callback list empty!\n" , __FUNCTION__);
    return NULL;
  }

  //pop last one
  if(pstEnv->cb_count == 1)
  {
    pstCBInfo = pstEnv->cb_head;
    pstEnv->cb_head = NULL;
    pstEnv->cb_tail = NULL;
    pstEnv->cb_count--;
    return pstCBInfo;
  }

  //pop head
  pstCBInfo = pstEnv->cb_head;
  pstEnv->cb_head = pstEnv->cb_head->next;
  pstEnv->cb_count--;
  return pstCBInfo;
}

static void _print_space()
{
  REDISENV *penv = NULL;
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  int i = 0;
  int sld = -1;
  int real_len = 0;
  

  if(pspace->slog_d < 0)
    return;

  sld = pspace->slog_d;
  //real_len
  if(pspace->env_list && pspace->list_len>=0)
    real_len = (int)pow(2 , pspace->list_len);

  
  //PRINT 
  slog_log(sld, SL_VERBOSE , "(((=============PRINT=============");
  slog_log(sld , SL_VERBOSE , "----valid_count:%d list_len:%d----" , pspace->valid_count , pspace->list_len);
  for(i=0; i<real_len; i++)
  {
    penv = &pspace->env_list[i];
    slog_log(sld, SL_VERBOSE, "stat:%d id:%d ip:%s port:%d cb_count:%d", penv->stat , penv->id , penv->ip , 
      penv->port , penv->cb_count);
  }
  slog_log(sld, SL_VERBOSE , "=============END=============)))");

  return;
}

static REDISENV *_rd2env(int rd , const char *caller)
{
  REDISENV *penv = NULL;
  REDIS_GLOBALSPACE *pspace = &redis_global_space;
  int real_len = 0;
  int sld = -1;
  int i;

  /***Arg Check*/
  sld = pspace->slog_d;
  if(sld < 0) //not opened yet
    return NULL;

  if(!caller)
  {
    slog_log(sld, SL_ERR, "<%s> failed! caller nil! rd:%d", __FUNCTION__ , rd);
    return NULL;
  }

  //empty list
  if(!pspace->env_list || pspace->list_len<0)
  {
    slog_log(sld, SL_ERR, "<%s> failed! Empty List! rd:%d caller:%s", __FUNCTION__ , rd , caller);
    return NULL;
  }

  //rd
  real_len = (int)pow(2 , pspace->list_len);
  if(rd<0 || rd>=real_len)
  {
    slog_log(sld, SL_ERR, "<%s> failed! rd illegal! rd:%d real_len:%d caller:%s", __FUNCTION__ , rd , 
      real_len , caller);
    return NULL;  
  }

  /***Get Env*/
  penv = &pspace->env_list[rd];
  if(penv->stat == REDIS_ENV_STAT_EMPTY)
  {
    slog_log(sld, SL_ERR, "<%s> failed! rd not used! rd:%d caller:%s", __FUNCTION__ , rd , caller);
    return NULL;
  }

  if(penv->id != rd)
  {
    slog_log(sld, SL_ERR, "<%s> failed! rd not match! rd:%d id:%d caller:%s", __FUNCTION__ , rd , 
      penv->id , caller);
    return NULL;
  }

  //slog_log(sld, SL_VERBOSE , "<%s> success! rd:%d caller:%s", __FUNCTION__ , rd , caller);
  return penv;
}

//Clear most member. except stat,id
static int _reset_env(REDISENV *penv)
{
  CBINFO *pcb = NULL;
  /***Arg Check*/
  if(!penv)
    return -1;

  //Clear
  memset(penv->ip , 0 , sizeof(penv->ip));
  penv->port = 0;
  penv->hiredis_cxt = NULL;
  penv->flag = REDIS_CONN_FLG_NONE;
  while(penv->cb_count > 0)
  {
    pcb = _hpop_cbi(penv);     
    _free_cb(pcb);
  }
  penv->cb_count = 0;
  penv->cb_head = NULL;
  penv->cb_tail = NULL;
  penv->connect_end_ts = 0;
  penv->timeout = 0;

  return 0;
}

//clear connection info
static int _redis_disconnect(int rd)
{
  REDISENV *pstEnv = NULL;
  pstEnv = NULL;
  int sld = -1;
  REDIS_GLOBALSPACE *pspace = &redis_global_space;  
  CBINFO *pcb = NULL;

  /***Check Basic*/
  sld = pspace->slog_d;
  if(sld < 0)
    return -1;

  /***Get Env*/
  pstEnv = _rd2env(rd, __FUNCTION__);
  if(!pstEnv)
    return -1;

  /***Handle*/
  //free hiredis info
  if(pstEnv->hiredis_cxt)
  {
    redisFree(pstEnv->hiredis_cxt);
    pstEnv->hiredis_cxt = NULL;
  }

  //free callback info
  while(pstEnv->cb_count > 0)
  {
    pcb = _hpop_cbi(pstEnv);     
    _free_cb(pcb);
  }
  pstEnv->cb_count = 0;
  pstEnv->cb_head = NULL;
  pstEnv->cb_tail = NULL;
   
  slog_log(sld , SL_INFO , "<%s> success! rd:%d" , __FUNCTION__ , rd);
  return 0;
}

static void _free_cb(CBINFO *pcb)
{
  if(!pcb)
    return;

  if(pcb->private_len > DEFAULT_CB_PRIVATE_LEN)
    free(pcb->private);
  free(pcb);

  return;
}
