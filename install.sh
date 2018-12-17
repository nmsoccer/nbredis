#!/bin/bash
HEADER_DIR=/usr/local/include/nbredis
LIB_DIR=/usr/local/lib/

HEADER_NAME="redis_non_block.h"
SO_NAME=libnbredis.so
LINK_NAME="${SO_NAME}.0.1"
SRC_FILE="redis_non_block.c"
WORK_DIR=`pwd`

#echo "usage $0 [install] to compile and install"


#if [[ -n $1 && $1 == "install" ]]
#then
function install()
{
  echo "try to install..."
  gcc -fPIC -shared ${SRC_FILE} -o ${SO_NAME}
  
  mkdir -p ${HEADER_DIR}
  if [[ $? -ne 0 ]]
  then
    #echo "create ${HEADER_DIR} failed!"
    exit 1 
  fi

  cp ${HEADER_NAME} ${HEADER_DIR}
  if [[ $? -ne 0 ]]
  then
    exit 1
  fi

  cp ${SO_NAME} ${LIB_DIR}
  if [[ $? -ne 0 ]]
  then
    exit 1
  fi

  cd ${LIB_DIR}
  rm ${LINK_NAME}
  ln -s ${SO_NAME} ${LINK_NAME}
  if [[ $? -ne 0 ]]
  then
    exit 1
  fi

  cd ${WORK_DIR}
  rm ${SO_NAME}

  echo "install success!"
}

install 
