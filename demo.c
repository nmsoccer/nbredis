#include <stdio.h>
#include <stdlib.h>
#include <nbredis/redis_non_block.h>
#include <signal.h>

#define MAX_REDIS_CONNECT 2
int rd_array[MAX_REDIS_CONNECT] = {-1};

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
     //redis_exec(rd_array[0] , "HGET user_info name" , my_callback , "HGET:" , 5);
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
    //counting
  }
  //free
  return 0;
}

