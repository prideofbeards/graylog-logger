//
//  GraylogConnection.cpp
//  dm-graylog-logger
//
//  Created by Jonas Nilsson on 2016-12-29.
//  Copyright © 2016 European Spallation Source. All rights reserved.
//

#include "graylog_logger/GraylogConnection.hpp"
#include <cassert>
#include <chrono>
#include <ciso646>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <sys/types.h>
#include <utility>

#ifdef _WIN32
#include <Ws2def.h>
#include <windows.h>
#include <winsock2.h>
#define poll WSAPoll
#define close closesocket
#define SHUT_RDWR SD_BOTH
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

GraylogConnection::GraylogConnection(const std::string &host, int port)
    : endWait(time(nullptr) + retryDelay), host(host),
      port(std::to_string(port)),
      retConState(GraylogConnection::ConStatus::NONE) {
#ifdef _WIN32
  WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
  connectionThread = std::thread(&GraylogConnection::ThreadFunction, this);
}

GraylogConnection::~GraylogConnection() {
#ifdef _WIN32
  WSACleanup();
#endif
  EndThread();
}

GraylogConnection::ConStatus GraylogConnection::GetConnectionStatus() {
  return retConState;
}

void GraylogConnection::EndThread() {
  closeThread = true;
  connectionThread.join();
  if (nullptr != conAddresses) {
    freeaddrinfo(conAddresses);
    conAddresses = nullptr;
  }
}

void GraylogConnection::MakeConnectionHints() {
  std::memset(reinterpret_cast<void *>(&hints), 0, sizeof(hints));
  // hints.ai_family = AF_UNSPEC; //Accept both IPv4 and IPv6
  hints.ai_family = AF_INET;       // Accept only IPv4
  hints.ai_socktype = SOCK_STREAM; // Use TCP
  hints.ai_protocol = IPPROTO_TCP;
}

void GraylogConnection::GetServerAddr() {
  if (nullptr != conAddresses) {
    freeaddrinfo(conAddresses);
    conAddresses = nullptr;
  }
  int res = getaddrinfo(host.c_str(), port.c_str(), &hints, &conAddresses);
  if (-1 == res or nullptr == conAddresses) {
    // std::cout << "GL: Failed to get addr.-info." << std::endl;
    SetState(ConStatus::ADDR_RETRY_WAIT);
    endWait = time(nullptr) + retryDelay;
  } else {
    // std::cout << "GL: Changing state to CONNECT." << std::endl;
    SetState(ConStatus::CONNECT);
  }
}
void GraylogConnection::ConnectToServer() {
  addrinfo *p;
  int response;
  for (p = conAddresses; p != nullptr; p = p->ai_next) {
    // std::cout << "GL: Connect addr. iteration." << std::endl;
    socketFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (-1 == socketFd) {
      continue;
    }
#ifdef SO_NOSIGPIPE
    int value = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value));
#endif
    // setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, const void *, socklen_t);

#ifdef _WIN32
    u_long iMode = 1;
    response = ioctlsocket(socketFd, FIONBIO, &iMode);
#else
    response = fcntl(socketFd, F_SETFL, O_NONBLOCK);
#endif
    if (-1 == response) {
      close(socketFd);
      socketFd = -1;
      continue;
    }
    response = connect(socketFd, p->ai_addr, p->ai_addrlen);
#ifdef _WIN32
    if (-1 == response and WSAGetLastError() == WSAEWOULDBLOCK) {
#else
    if (-1 == response and EINPROGRESS == errno) {
#endif
      // std::cout << "GL: Now waiting for connection." << std::endl;
      SetState(ConStatus::CONNECT_WAIT);
      break;
    }
    close(socketFd);
    socketFd = -1;
  }
  if (-1 == socketFd) {
    // std::cout << "GL: Failed all connections." << std::endl;
    connectionTries++;
    SetState(ConStatus::CONNECT_RETRY_WAIT);
  }
  endWait = time(nullptr) + retryDelay;
}

void GraylogConnection::ConnectWait() {
  if (endWait < time(nullptr)) {
    // std::cout << "GL: Timeout on waiting for connection." << std::endl;
    SetState(ConStatus::CONNECT);
    close(socketFd);
    socketFd = -1;
    connectionTries++;
    return;
  }
  timeval selectTimeout{0};
  selectTimeout.tv_sec = 0;
  selectTimeout.tv_usec = 50000;
  fd_set exceptfds{{0}};
  FD_ZERO(&exceptfds);
  FD_SET(socketFd, &exceptfds);
  fd_set writefds;
  FD_ZERO(&writefds);
  FD_SET(socketFd, &writefds);
  int changes =
      select(socketFd + 1, nullptr, &writefds, &exceptfds, &selectTimeout);
  // std::cout << "GL: select changes: " << changes << std::endl;
  if (1 == changes) {
    if (FD_ISSET(socketFd, &exceptfds)) {
      SetState(ConStatus::CONNECT);
      close(socketFd);
      socketFd = -1;
      connectionTries++;
    } else if (FD_ISSET(socketFd, &writefds)) {
      bytesSent = 0;
      currentMessage = "";
      firstMessage = true;
      SetState(ConStatus::NEW_MESSAGE);
    }
  } else if (0 == changes) {
    // timeout
  } else if (-1 == changes) {
    SetState(ConStatus::CONNECT);
    close(socketFd);
    socketFd = -1;
    connectionTries++;
    return;
  } else if (1 == changes and not FD_ISSET(socketFd, &writefds)) {
    assert(false); // Should never be reached
  }
}

void GraylogConnection::NewMessage() {
  // std::cout << "GL: Waiting for message." << std::endl;

  if (logMessages.time_out_peek(currentMessage, 50)) {
    bytesSent = 0;
    // std::cout << "GL: Got message to send." << std::endl;
    SetState(ConStatus::SEND_LOOP);
  }
}

void GraylogConnection::SendMessage(std::string msg) { logMessages.push(msg); }

void GraylogConnection::CheckConnectionStatus() {
  //    timeval selectTimeout;
  //    selectTimeout.tv_sec = 0;
  //    selectTimeout.tv_usec = 0;
  //    fd_set exceptfds;
  //    FD_ZERO(&exceptfds);
  //    FD_SET(socketFd, &exceptfds);
  //    fd_set readfds;
  //    FD_ZERO(&readfds);
  //    FD_SET(socketFd, &readfds);
  //    fd_set writefds;
  //    FD_ZERO(&writefds);
  //    FD_SET(socketFd, &writefds);
  //    int selRes = select(socketFd + 1, &readfds, nullptr, &exceptfds,
  //    &selectTimeout);
  //    //std::cout << "Connection check: " << selRes << std::endl;
  //    if(selRes and FD_ISSET(socketFd, &exceptfds)) {
  //        SetState(ConStatus::CONNECT);
  //        shutdown(socketFd, SHUT_RDWR);
  //        close(socketFd);
  //        return;
  //    }
  //    char b;
  //    if (FD_ISSET(socketFd, &readfds)) {
  //        ssize_t peekRes = recv(socketFd, &b, 1, MSG_PEEK);
  //        //std::cout << "Peek check: " << peekRes << std::endl;
  //        if (-1 == peekRes) {
  //            //perror("peek");
  //            SetState(ConStatus::CONNECT);
  //            shutdown(socketFd, SHUT_RDWR);
  //            close(socketFd);
  //            return;
  //        }
  //    }
  pollfd ufds[1];
  ufds[0].fd = socketFd;
#ifdef POLLRDHUP
  ufds[0].events = POLLIN | POLLOUT | POLLRDHUP;
#else
  ufds[0].events = POLLIN | POLLOUT;
#endif
  int pollRes = poll(ufds, 1, 10);
  // std::cout << "Number of poll results: " << pollRes << std::endl;
  if (pollRes == 1) {
    // std::cout << "Poll result: " << ufds[0].revents << std::endl;
    if (ufds[0].revents & POLLERR) {
      SetState(ConStatus::CONNECT);
      shutdown(socketFd, SHUT_RDWR);
      close(socketFd);
      socketFd = -1;
      connectionTries++;
// std::cout << "Got poll error." << std::endl;
#ifdef POLLRDHUP
    } else if (ufds[0].revents & POLLHUP or ufds[0].revents & POLLRDHUP) {
#else
    } else if (ufds[0].revents & POLLHUP) {
#endif
      // std::cout << "Got poll hup." << std::endl;
      SetState(ConStatus::CONNECT);
      shutdown(socketFd, SHUT_RDWR);
      close(socketFd);
    } else if (ufds[0].revents & POLLNVAL) {
      // std::cout << "Got poll nval." << std::endl;
    }
  }
}

void GraylogConnection::SendMessageLoop() {
  timeval selectTimeout{0};
  selectTimeout.tv_sec = 0;
  selectTimeout.tv_usec = 50000;

  fd_set exceptfds{{0}};
  FD_ZERO(&exceptfds);
  FD_SET(socketFd, &exceptfds);
  fd_set writefds{{0}};
  FD_ZERO(&writefds);
  FD_SET(socketFd, &writefds);
  int selRes =
      select(socketFd + 1, nullptr, &writefds, &exceptfds, &selectTimeout);
  // std::cout << "GL: Send message changes: " << selRes << std::endl;
  if (selRes > 0) {
    if (FD_ISSET(socketFd, &exceptfds)) { // Some error
      // std::cout << "GL: Got send msg exception." << std::endl;
      SetState(ConStatus::CONNECT);
      shutdown(socketFd, SHUT_RDWR);
      close(socketFd);
      return;
    }
    if (FD_ISSET(socketFd, &writefds)) {
      // std::cout << "GL: Ready to write." << std::endl;
      {
        ssize_t cBytes = send(
            socketFd,
            currentMessage.substr(bytesSent, currentMessage.size() - bytesSent)
                .c_str(),
            currentMessage.size() - bytesSent + 1, sendOpt);
        // std::cout << "GL: Sent bytes: " << cBytes << std::endl;
        if (-1 == cBytes) {
#ifdef _WIN32
          if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
          if (EAGAIN == errno or EWOULDBLOCK == errno) {
#endif
            // std::cout << "GL: Non blocking, got errno:" << errno <<
            // std::endl;
            // Should probably handle this
          } else {
            // std::cout << "GL: Send error with errno:" << errno << std::endl;
            // perror("send:");
            SetState(ConStatus::CONNECT);
            shutdown(socketFd, SHUT_RDWR);
            close(socketFd);
            return;
          }
        } else if (0 == cBytes) {
          // std::cout << "No bytes sent." << std::endl;
          // Do nothing
        } else {
          bytesSent += cBytes;
          if (bytesSent == currentMessage.size() + 1) {
            bool popResult = logMessages.try_pop();
            assert(popResult);
            SetState(ConStatus::NEW_MESSAGE);
          }
        }
      }
    }
  } else if (0 == selRes) {
    // std::cout << "Got send timeout." << std::endl;
    // timeout
  } else if (-1 == selRes) {
    // error
  } else {
    assert(false); // Should never be reached
  }
}

void GraylogConnection::ThreadFunction() {
  auto sleepTime = std::chrono::milliseconds(50);
  SetState(ConStatus::ADDR_LOOKUP);
  bytesSent = 0;
  MakeConnectionHints();
  while (true) {
    if (closeThread) {
      if (-1 != socketFd) {
        shutdown(socketFd, SHUT_RDWR);
        close(socketFd);
      }
      break;
    }
    if (5 < connectionTries) {
      SetState(ConStatus::ADDR_RETRY_WAIT);
      connectionTries = 0;
      endWait = time(nullptr) + retryDelay;
    }
    switch (stateMachine) {
    case ConStatus::ADDR_LOOKUP:
      GetServerAddr();
      break;
    case ConStatus::ADDR_RETRY_WAIT:
      if (time(nullptr) > endWait) {
        SetState(ConStatus::ADDR_LOOKUP);
      } else {
        std::this_thread::sleep_for(sleepTime);
      }
      break;
    case ConStatus::CONNECT:
      ConnectToServer();
      break;
    case ConStatus::CONNECT_RETRY_WAIT:
      if (time(nullptr) > endWait) {
        SetState(ConStatus::CONNECT);
      } else {
        std::this_thread::sleep_for(sleepTime);
      }
      break;
    case ConStatus::CONNECT_WAIT:
      ConnectWait();
      break;
    case ConStatus::NEW_MESSAGE:
      NewMessage();
      CheckConnectionStatus();
      break;
    case ConStatus::SEND_LOOP:
      SendMessageLoop();
      break;
    default:
      assert(false); // This should never be reached
      break;
    }
  }
}

void GraylogConnection::SetState(GraylogConnection::ConStatus newState) {
  stateMachine = newState;
  retConState = newState;
}
