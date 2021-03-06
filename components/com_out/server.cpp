#include "server.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <thread>
#include <chrono>
#include <kj/io.h>
#include "utilities/json.hpp"
#include "frame/config.h"


com_out::Server::Server(serialize::AppState& appState) : _appState(appState), _pollOutput(true), _newClient(false) {
  _buf = new char[_bufSize];
}

com_out::Server::~Server() {
  delete _buf;
}

void com_out::Server::run() {
  create();
  serve();
}

void com_out::Server::stop() {
  close(_server);
  _pollOutput = false;
}

void com_out::Server::pollOutput() {
  while(_pollOutput) {
    std::lock_guard<std::mutex> lockGuard(_newClientMtx);
    if (_appState.isDirty() || _newClient) {
      // const auto startTime = std::chrono::high_resolution_clock::now();

      // Broadcast Frame data
      kj::VectorOutputStream stream;
      if (_appState.writeToStream(stream)) {
        const int len = stream.getArray().size();
        const BYTE* buf = stream.getArray().begin();
        broadcast(buf, len);
      }

      // Broadcast ctrlData
      nlohmann::json ctrlDataMsg = {
        {"type", "server.ctrlData"},
        {"data", _appState.getCtrlData()},
      };
      broadcast(ctrlDataMsg.dump());
      _newClient = false;

      // const auto endTime = std::chrono::high_resolution_clock::now();
      // const auto durAlgo = std::chrono::duration<double, std::milli>(endTime - startTime);
      // std::cout << "Send Frame: " << durAlgo.count() << " [ms]" << std::endl;
    }
    // Polling every 2 ms to check if there is new data
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void com_out::Server::registerRequestListener(std::shared_ptr<IRequestListener> listener) {
  _requestListeners.push_back(listener);
}

void com_out::Server::deleteRequestListener(std::shared_ptr<IRequestListener> listener) {
  _requestListeners.erase(std::remove(_requestListeners.begin(), _requestListeners.end(), listener), _requestListeners.end());
}

void com_out::Server::serve() {
  int client;
  struct sockaddr_in clientAddr;
  socklen_t clientlen = sizeof(clientAddr);

  // accept clients
  while((client = accept(_server, (struct sockaddr *)&clientAddr, &clientlen)) > 0) {
    // brackets needed to remove locks after adding to _clients
    {
      std::lock_guard<std::mutex> lockGuard1(_newClientMtx);
      std::lock_guard<std::mutex> lockGuard2(_clientsMtx);
      std::cout << "Add client: " << client << std::endl;
      _clients.push_back(client);
      _newClient = true;
    }
    std::thread handleClientThread(&com_out::Server::handle, this, client);
    handleClientThread.detach();
  }

  closeSocket();
}

void com_out::Server::handle(int client) {
  // loop to handle all requests
  for (;;) {
    // get a request
    std::string request = getRequest(client);
    // break if client is done or an error occurred
    if (request.empty()) {
      break;
    }

    auto jsonRequest = nlohmann::json::parse(request);
    nlohmann::json jsonResponse = {
      {"type", "server.callback"},
      {"cbIndex", jsonRequest.value("cbIndex", -1)}, // in case cbIndex does not exist, default value of -1 is used
      {"data", {}},
    };

    // std::cout << "Request: " << request << std::endl;

    // pass to every request listener and collect responses
    for(auto const& listener: _requestListeners) {
      void(listener->handleRequest(jsonRequest["type"], jsonRequest, jsonResponse["data"]));
    }

    // std::cout << "Response: " << jsonResponse.dump() << std::endl;

    auto [msg, len] = createMsg(jsonResponse.dump());
    const bool success = sendToClient(client, msg, len);
    delete [] msg;
    if (!success) {
      break;
    }
  }

  // remove client
  std::lock_guard<std::mutex> lockGuard(_clientsMtx);
  std::cout << "Remove client: " << client << std::endl;
  _clients.erase(std::remove(_clients.begin(), _clients.end(), client), _clients.end());
  close(client);
}

std::string com_out::Server::getRequest(int client) {
  std::string request = "";

  // read until we get a newline
  while(request.find("\n") == std::string::npos) {
    int nread = recv(client, _buf, _bufSize, 0);
    if (nread < 0) {
      if (errno == EINTR)
        // the socket call was interrupted -- try again
        continue;
      else
        // an error occurred, so break out
        return "";
    }
    else if(nread == 0) {
      // the socket is closed
      return "";
    }
    // be sure to use append in case we have binary data
    request.append(_buf, nread);
  }

  // a better server would cut off anything after the newline and
  // save it in a cache
  return request;
}

bool com_out::Server::sendToClient(int client, const BYTE* buf, const int len) {
  // prepare to send response
  int nleft = len;
  int nwritten;
  // loop to be sure it is all sent
  while(nleft) {
    try {
      {
        // Needed because otherwise there can be trouble if client is removed while send is active
        std::lock_guard<std::mutex> lockGuard(_clientsMtx);
        nwritten = send(client, buf, nleft, 0);
      }
      if(nwritten < 0) {
        if (errno == EINTR) {
          // the socket call was interrupted -- try again
          continue;
        } 
        else {
          // an error occurred, so break out
          return false;
        }
      } 
      else if(nwritten == 0) {
        // the socket is closed
        return false;
      }
      nleft -= nwritten;
      buf += nwritten;
    }
    catch (const std::exception& e) {
      return false;
    }
  }
  return true;
}

void com_out::Server::broadcast(const std::string payload) {
  auto [msg, msgSize] = createMsg(payload);
  for(int client: _clients) {
    sendToClient(client, msg, msgSize);
  }
  delete [] msg;
}

std::tuple<BYTE*, int> com_out::Server::createMsg(const std::string payload) const {
  const int payloadSize = payload.size();

  // create msg bytes 
  const int msgSize = _headerSize + payloadSize;
  auto* msg = new BYTE[msgSize];
  memset(msg, 0x00, msgSize);

  // start bytes [0-1]
  msg[0] = 0x0F;
  msg[1] = 0xF0;
  // size bytes [2-5] in big endian
  msg[5] = static_cast<BYTE>(payloadSize & 0xFF);
  msg[4] = static_cast<BYTE>((payloadSize >> 8) & 0xFF);
  msg[3] = static_cast<BYTE>((payloadSize >> 16) & 0xFF);
  msg[2] = static_cast<BYTE>((payloadSize >> 24) & 0xFF);

  // type byte [6]
  msg[6] = 0x01; // JSON string

  // interface verison byte [7]: major [8]: minor
  msg[7] = Config::interfaceVersionMajor & 0xFF;
  msg[8] = Config::interfaceVersionMinor & 0xFF;

  // copy string to msg
  memcpy(msg + _headerSize, payload.c_str(), payloadSize);

  return {msg, msgSize};
}

void com_out::Server::broadcast(const BYTE* payload, const int payloadSize) {
  auto [msg, msgSize] = createMsg(payload, payloadSize);
  for(int client: _clients) {
    sendToClient(client, msg, msgSize);
  }
  delete [] msg;
}

std::tuple<BYTE*, int> com_out::Server::createMsg(const BYTE* payload, const int payloadSize) const {
  // create msg bytes 
  const int msgSize = _headerSize + payloadSize;
  auto* msg = new BYTE[msgSize];
  memset(msg, 0x00, msgSize);

  // start bytes [0-1]
  msg[0] = 0x0F;
  msg[1] = 0xF0;
  // size bytes [2-5] in big endian
  msg[5] = static_cast<BYTE>(payloadSize & 0xFF);
  msg[4] = static_cast<BYTE>((payloadSize >> 8) & 0xFF);
  msg[3] = static_cast<BYTE>((payloadSize >> 16) & 0xFF);
  msg[2] = static_cast<BYTE>((payloadSize >> 24) & 0xFF);

  // type byte [6]
  msg[6] = 0x02; // capnp binary data

  // interface verison byte [7]: major [8]: minor
  msg[7] = Config::interfaceVersionMajor & 0xFF;
  msg[8] = Config::interfaceVersionMinor & 0xFF;

  // copy capnp binary data to msg
  memcpy(msg + _headerSize, payload, payloadSize);

  return {msg, msgSize};
}
