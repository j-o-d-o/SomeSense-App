#pragma once

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/un.h>

#include "server.h"


namespace com_out {
  class TcpServer : public Server {
  public:
    TcpServer();

  protected:
    void create();
    void closeSocket();

  private:
    static void interrupt(int);
    
    // static const char* _socketName;
  };
}