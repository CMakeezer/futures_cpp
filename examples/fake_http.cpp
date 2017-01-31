#include <futures/EventExecutor.h>
#include <futures/Signal.h>
#include <futures/Timer.h>
#include <futures/TcpStream.h>
#include <futures/http/HttpCodec.h>
#include <futures/io/PipelinedRpcFuture.h>
#include <futures/io/IoStream.h>
#include <thread>
#include <iostream>

using namespace futures;

class DummyService: public Service<http::Request, http::Response> {
public:
    BoxedFuture<http::Response> operator()(http::Request req) {
#ifndef NDEBUG
        std::cerr << req << std::endl;
        auto c = folly::makeMoveWrapper(std::move(req));
        return delay(EventExecutor::current(), 1.0)
          .andThen([c] (std::error_code ec) mutable {
              http::Response resp;
              resp.http_errno = 200;
              resp.body.append(std::move(c->body));
              resp.body.append("TESTX", 5);
              // resp.body = "XXXXX";
              return makeOk(std::move(resp));
          }).boxed();
#else
        http::Response resp;
        resp.body.append("TESTX", 5);
        return makeOk(std::move(resp)).boxed();
#endif
    }
};

#if 0
class BytesWriteSink :
  public AsyncSinkBase<BytesWriteSink, std::unique_ptr<folly::IOBuf>> {
public:
  using Out = std::unique_ptr<folly::IOBuf>;

  explicit BytesWriteSink(std::unique_ptr<io::Io> io)
    : io_(std::move(io)) {
  }

  Try<bool> startSend(Out& item) override {
    if (!q_)
      q_ = std::move(item);
    else
      q_->appendChain(std::move(item));
    return Try<bool>(true);
  }

  Poll<folly::Unit> pollComplete() override {
    if (!q_)
      return makePollReady(folly::Unit());
    if (!q_->countChainElements())
      return makePollReady(folly::Unit());
  }
private:
  std::unique_ptr<io::Io> io_;
  std::unique_ptr<folly::IOBuf> q_;
};

class BytesWriteSinkAdapter :
  public OutboundHandler<http::Response, std::unique_ptr<folly::IOBuf>> {
public:
  BoxedFuture<folly::Unit> write(Context* ctx, http::Response msg) override {
      // q_->appendChain(std::move(msg));
      // FUTURES_DLOG(INFO) << "HERE : " << q_->countChainElements();
      auto p = folly::IOBuf::create(5);
      sink_->startSend(p);
      return makeOk().boxed();
  }

  explicit BytesWriteSinkAdapter(BytesWriteSink* sink) : sink_(sink) {}

private:
  BytesWriteSink *sink_;
};
#endif

static BoxedFuture<folly::Unit> process(EventExecutor *ev,
    tcp::SocketPtr client,
    std::shared_ptr<DummyService> service) {
    return makeRpcFuture(
      io::FramedStream<http::HttpV1Decoder>(folly::make_unique<tcp::SocketIOHandler>(ev, client)),
      service,
      io::FramedSink<http::HttpV1Encoder>(folly::make_unique<tcp::SocketIOHandler>(ev, client))
    ).then([] (Try<folly::Unit> err) {
      if (err.hasException())
        std::cerr << err.exception().what() << std::endl;
      return makeOk();
    }).boxed();
}

class DummyWebsocketService: public Service<websocket::DataFrame, websocket::DataFrame> {
public:
    BoxedFuture<websocket::DataFrame> operator()(websocket::DataFrame req) override {
      if (req.getType() == websocket::DataFrame::HANDSHAKE) {
        std::cerr << *req.getHandshake() << std::endl;
        // resp.body.append("TESTX", 5);
        return makeOk(websocket::DataFrame::buildHandshakeResponse(
              *req.getHandshake())).boxed();
      } else {
        throw std::invalid_argument("unimpl");
      }
    }
};

static BoxedFuture<folly::Unit> processWs(EventExecutor *ev,
    tcp::SocketPtr client,
    std::shared_ptr<DummyWebsocketService> service) {
    return makeRpcFuture(
      io::FramedStream<websocket::RFC6455Decoder>(folly::make_unique<tcp::SocketIOHandler>(ev, client)),
      service,
      io::FramedSink<websocket::RFC6455Encoder>(folly::make_unique<tcp::SocketIOHandler>(ev, client))
    ).then([] (Try<folly::Unit> err) {
      if (err.hasException())
        std::cerr << err.exception().what();
      return makeOk();
    }).boxed();
}



int main(int argc, char *argv[])
{
    std::error_code ec;
    auto s = std::make_shared<tcp::Socket>();
    s->tcpServer("127.0.0.1", 8011, 32, ec);
    assert(!ec);
    const int kWorkers = 4;

    EventExecutor loop(true);
    std::unique_ptr<EventExecutor> worker_loops[kWorkers];
    auto pservice = std::make_shared<DummyService>();
    auto pWsservice = std::make_shared<DummyWebsocketService>();

    for (int i = 0; i < kWorkers; i++)
        worker_loops[i].reset(new EventExecutor());

    std::cerr << "listening: " << 8011 << std::endl;
    auto f = tcp::Stream::acceptStream(&loop, s)
        .forEach([&worker_loops, pservice, pWsservice] (tcp::SocketPtr client) {
                auto loop = worker_loops[rand() % kWorkers].get();
                // auto loop = EventExecutor::current();
                loop->spawn(process(loop, client, pservice));
                // loop->spawn(processWs(loop, client, pWsservice));
                })
        .then([] (Try<folly::Unit> err) {
            if (err.hasException())
                std::cerr << "Error: " << err.exception().what() << std::endl;
            return makeOk();
        });
    auto sig = signal(&loop, SIGINT)
        .andThen([&] (int signum) {
                std::cerr << "killed by " << signum << std::endl;
                EventExecutor::current()->stop();
                for (int i = 0; i < kWorkers; ++i) {
                worker_loops[i]->spawn(makeLazy([] () {
                            EventExecutor::current()->stop();
                            return folly::unit;
                        }));
                }
                return makeOk();
                });
    loop.spawn(std::move(f));
    loop.spawn(std::move(sig));

    std::vector<std::thread> workers;
    for (int i = 0 ;i <  kWorkers; ++i) {
        auto worker = std::thread([&worker_loops, i] () {
                worker_loops[i]->run(true);
                });
        workers.push_back(std::move(worker));
    }
    loop.run();
    for (auto &e: workers)
        e.join();

    return 0;
}
