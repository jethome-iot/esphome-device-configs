#include "common.h"

#include "esphome/components/modbus_tcp/modbus_tcp.h"
#include "esphome/core/hal.h"
#include "esphome/core/wake.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace esphome::modbus_tcp::testing {

// The component as the firmware runs it, with the retry made short enough to wait for.
class TestableServer : public ModbusTcpServer {
 public:
  void set_retry_interval(uint32_t ms) { this->retry_interval_ms_ = ms; }
};

// The component on a real loopback socket. Each pass waits on select() for the sockets the
// component registered, as the host's main loop does, then runs its loop().
class Sockets : public Server {
 protected:
  void SetUp() override {
    Server::SetUp();
    // A port of its own per case, so no case meets another's connections in TIME_WAIT.
    static uint16_t next = 20000 + getpid() % 20000;
    this->port = next++;
    this->tcp.set_server(&this->server);
    this->tcp.set_port(this->port);
  }

  void TearDown() override {
    for (int fd : this->fds_)
      ::close(fd);
    this->tcp.on_shutdown();
  }

  void pump(int passes = 1) {
    for (int i = 0; i < passes; i++) {
      esphome::internal::wakeable_delay(2);
      this->tcp.loop();
    }
  }

  /// A connected client, or -1 with errno set when nothing listens.
  int connect_to(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0) {
      const int err = errno;
      ::close(fd);
      errno = err;
      return -1;
    }
    this->fds_.push_back(fd);
    return fd;
  }
  int connect() { return this->connect_to(this->port); }

  static void send_all(int fd, const Bytes &bytes) {
    ASSERT_EQ(::send(fd, bytes.data(), bytes.size(), 0), static_cast<ssize_t>(bytes.size()));
  }

  /// Up to @p count bytes from the server, pumping it while they come.
  Bytes receive(int fd, size_t count, int passes = 500) {
    Bytes out;
    uint8_t buffer[512];
    for (int i = 0; i < passes && out.size() < count; i++) {
      this->pump();
      const ssize_t got = ::recv(fd, buffer, std::min(sizeof(buffer), count - out.size()), MSG_DONTWAIT);
      if (got == 0)
        break;
      if (got > 0)
        out.insert(out.end(), buffer, buffer + got);
    }
    return out;
  }

  /// Whether the server closes the connection within @p passes.
  bool closed(int fd, int passes = 500) {
    uint8_t buffer[64];
    for (int i = 0; i < passes; i++) {
      this->pump();
      const ssize_t got = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
        return true;
    }
    return false;
  }

  /// A read of the six relays as transaction @p transaction, and the answer it gets.
  static Bytes read_relays(uint16_t transaction) {
    return {static_cast<uint8_t>(transaction >> 8), static_cast<uint8_t>(transaction), 0, 0, 0, 6, 1, 0x01, 0, 0, 0, 6};
  }
  static Bytes relays_answer(uint16_t transaction, uint8_t bits) {
    return {static_cast<uint8_t>(transaction >> 8), static_cast<uint8_t>(transaction), 0, 0, 0, 4, 1, 0x01, 1, bits};
  }

  TestableServer tcp;
  uint16_t port{0};

 private:
  std::vector<int> fds_;
};

TEST_F(Sockets, ListensOnlyOnceSwitchedOn) {
  this->pump(3);
  EXPECT_EQ(this->connect(), -1);
  EXPECT_EQ(errno, ECONNREFUSED);
  this->tcp.set_enabled(true);
  this->pump();
  EXPECT_NE(this->connect(), -1);
}

TEST_F(Sockets, AnswersARequest) {
  this->relays = {true, false, false, false, false, true};
  this->tcp.set_enabled(true);
  this->pump();
  const int fd = this->connect();
  send_all(fd, read_relays(7));
  EXPECT_EQ(this->receive(fd, 10), relays_answer(7, 0b100001));
}

TEST_F(Sockets, JoinsARequestSplitAcrossWrites) {
  this->tcp.set_enabled(true);
  this->pump();
  const int fd = this->connect();
  const Bytes request = read_relays(1);
  send_all(fd, Bytes(request.begin(), request.begin() + 5));
  this->pump(5);
  send_all(fd, Bytes(request.begin() + 5, request.end()));
  EXPECT_EQ(this->receive(fd, 10), relays_answer(1, 0));
}

// More than the component answers in one pass, and more than its 260-byte buffer takes in one
// read, so requests are cut across reads: the rest wait in the socket and come in order.
TEST_F(Sockets, AnswersPipelinedRequestsInOrder) {
  this->tcp.set_enabled(true);
  this->pump();
  const int fd = this->connect();
  Bytes burst;
  Bytes expected;
  for (uint16_t t = 1; t <= 40; t++) {
    const Bytes request = read_relays(t);
    const Bytes answer = relays_answer(t, 0);
    burst.insert(burst.end(), request.begin(), request.end());
    expected.insert(expected.end(), answer.begin(), answer.end());
  }
  send_all(fd, burst);
  EXPECT_EQ(this->receive(fd, expected.size()), expected);
}

TEST_F(Sockets, RefusesAClientPastTheLimit) {
  this->tcp.set_max_clients(2);
  this->tcp.set_enabled(true);
  this->pump();
  const int first = this->connect();
  const int second = this->connect();
  this->pump(3);
  const int third = this->connect();
  EXPECT_TRUE(this->closed(third));
  EXPECT_EQ(this->tcp.client_count(), 2u);
  send_all(second, read_relays(2));
  EXPECT_EQ(this->receive(second, 10), relays_answer(2, 0));
  send_all(first, read_relays(1));
  EXPECT_EQ(this->receive(first, 10), relays_answer(1, 0));
}

// A burst of connections is taken a few per pass, so it cannot hold the loop, and none is lost.
TEST_F(Sockets, TakesABurstOfConnectionsAFewPerPass) {
  this->tcp.set_max_clients(8);
  this->tcp.set_enabled(true);
  this->pump();
  int fds[8];
  for (int &fd : fds)
    fd = this->connect();
  this->pump();
  EXPECT_EQ(this->tcp.client_count(), 4u);
  this->pump();
  EXPECT_EQ(this->tcp.client_count(), 8u);
  for (uint16_t i = 0; i < 8; i++) {
    send_all(fds[i], read_relays(i));
    EXPECT_EQ(this->receive(fds[i], 10), relays_answer(i, 0));
  }
}

// The one slot is taken by a client that leaves and comes back before the next pass.
TEST_F(Sockets, GivesADepartedClientsSlotBack) {
  this->tcp.set_max_clients(1);
  this->tcp.set_enabled(true);
  this->pump();
  const int before = this->connect();
  send_all(before, read_relays(1));
  ASSERT_EQ(this->receive(before, 10), relays_answer(1, 0));
  ::shutdown(before, SHUT_RDWR);
  const int after = this->connect();
  send_all(after, read_relays(2));
  EXPECT_EQ(this->receive(after, 10), relays_answer(2, 0));
}

TEST_F(Sockets, DropsAnIdleClient) {
  this->tcp.set_idle_timeout(100);
  this->tcp.set_enabled(true);
  this->pump();
  const int fd = this->connect();
  this->pump();
  const uint32_t start = millis();
  ASSERT_TRUE(this->closed(fd, 1000));
  EXPECT_GE(millis() - start, 90u);
  EXPECT_EQ(this->tcp.client_count(), 0u);
}

// A client that has connected and not sent anything yet stays for the idle timeout.
TEST_F(Sockets, KeepsANewClientThatHasNotSentYet) {
  this->tcp.set_enabled(true);
  this->pump();
  const int fd = this->connect();
  this->pump(20);
  EXPECT_EQ(this->tcp.client_count(), 1u);
  send_all(fd, read_relays(3));
  EXPECT_EQ(this->receive(fd, 10), relays_answer(3, 0));
}

TEST_F(Sockets, ClosesOnAHeaderThatIsNotModbus) {
  this->tcp.set_enabled(true);
  this->pump();
  const int fd = this->connect();
  Bytes request = read_relays(1);
  request[3] = 1;  // protocol id 1
  send_all(fd, request);
  EXPECT_TRUE(this->closed(fd));
}

TEST_F(Sockets, StopsAndDropsItsClientsWhenSwitchedOff) {
  this->tcp.set_enabled(true);
  this->pump();
  const int fd = this->connect();
  this->pump();
  this->tcp.set_enabled(false);
  EXPECT_TRUE(this->closed(fd));
  EXPECT_EQ(this->connect(), -1);
}

TEST_F(Sockets, MovesToANewPort) {
  this->tcp.set_enabled(true);
  this->pump();
  const uint16_t moved = this->port + 1000;
  this->tcp.set_port(moved);
  this->pump();
  EXPECT_EQ(this->connect(), -1);
  const int fd = this->connect_to(moved);
  ASSERT_NE(fd, -1);
  send_all(fd, read_relays(4));
  EXPECT_EQ(this->receive(fd, 10), relays_answer(4, 0));
}

// Another server holds the port: the component listens once it is free again.
TEST_F(Sockets, TriesAgainWhileThePortIsTaken) {
  const int holder = ::socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(this->port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  ASSERT_EQ(::bind(holder, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(holder, 1), 0);
  this->tcp.set_retry_interval(50);
  this->tcp.set_enabled(true);
  this->pump(3);
  ::close(holder);
  const uint32_t start = millis();
  while (millis() - start < 2000) {
    this->pump();
    const int fd = this->connect();
    if (fd == -1)
      continue;
    send_all(fd, read_relays(5));
    EXPECT_EQ(this->receive(fd, 10), relays_answer(5, 0));
    return;
  }
  FAIL() << "never listened once the port was free";
}

}  // namespace esphome::modbus_tcp::testing
