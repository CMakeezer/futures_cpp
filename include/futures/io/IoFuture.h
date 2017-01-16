#pragma once

#include <futures/io/Io.h>
#include <futures/Future.h>
#include <futures/Stream.h>
#include <futures/AsyncSink.h>
#include <futures/EventExecutor.h>

namespace futures {
namespace io {

template <typename CODEC>
class FramedStream :
    public StreamBase<FramedStream<CODEC>, typename CODEC::In> {
public:
    using Item = typename CODEC::In;

    const static size_t kRdBufSize = 16 * 1024;

    Poll<Optional<Item>> poll() override {
        while (true) {
            if (readable_) {
                if (eof_) {
                    if (!rdbuf_->length())
                        return makePollReady(Optional<Item>());
                    auto f = codec_.decode_eof(rdbuf_);
                    if (f.hasException())
                        return Poll<Optional<Item>>(f.exception());
                    return makePollReady(Optional<Item>(folly::moveFromTry(f)));
                }
                auto f = codec_.decode(rdbuf_);
                if (f.hasException())
                    return Poll<Optional<Item>>(f.exception());
                if (f->hasValue()) {
                    return makePollReady(folly::moveFromTry(f));
                } else {
                    readable_ = false;
                }
            }
            assert(!eof_);
            // XXX use iovec
            rdbuf_->unshare();
            if (rdbuf_->headroom())
                rdbuf_->retreat(rdbuf_->headroom());
            if (!rdbuf_->tailroom())
                rdbuf_->reserve(0, kRdBufSize);
            std::error_code ec;
            ssize_t len = io_->read(rdbuf_.get(), rdbuf_->tailroom(), ec);
            if (ec == std::make_error_code(std::errc::connection_aborted)) {
                assert(len == 0);
                eof_ = true;
                readable_ = true;
            } else if (ec) {
                io_.reset();
                return Poll<Optional<Item>>(IOError("read frame", ec));
            } else if (len == 0) {
                if (io_->poll_read().isReady()) {
                    continue;
                } else {
                    return Poll<Optional<Item>>(not_ready);
                }
            } else {
                assert(len > 0);
                rdbuf_->append(len);
                readable_ = true;
            }
        }
    }

    FramedStream(std::unique_ptr<Io> io)
        : io_(std::move(io)), eof_(false), readable_(false),
          rdbuf_(folly::IOBuf::create(kRdBufSize)) {
    }
private:
    std::unique_ptr<Io> io_;
    CODEC codec_;
    bool eof_;
    bool readable_;
    std::unique_ptr<folly::IOBuf> rdbuf_;
};

template <typename CODEC>
class FramedSink :
    public AsyncSinkBase<FramedSink<CODEC>, typename CODEC::Out> {
public:
    using Out = typename CODEC::Out;
    const static size_t kWrBufSize = 16 * 1024;

    FramedSink(std::unique_ptr<Io> io)
        : io_(std::move(io)),
          wrbuf_(folly::IOBuf::create(kWrBufSize)) {
    }

    StartSend<Out> startSend(Out&& item) override {
        if (wrbuf_->length() > kWrBufSize) {
            auto r = pollComplete();
            if (r.hasException())
                return StartSend<Out>(r.exception());
            if (wrbuf_->length() > kWrBufSize) {
                FUTURES_DLOG(WARNING) << "buffer still full, reject sending frame";
                return StartSend<Out>(folly::make_optional(std::move(item)));
            }
        }

        auto r = codec_.encode(item, wrbuf_);
        if (r.hasException())
            return StartSend<Out>(r.exception());
        auto e = pollComplete();
        if (e.hasException())
            return StartSend<Out>(e.exception());
        return StartSend<Out>(folly::none);
    }

    Poll<folly::Unit> pollComplete() override {
        FUTURES_DLOG(INFO) << "flushing frame";
        while (wrbuf_->length()) {
            std::error_code ec;
            ssize_t len = io_->write(*wrbuf_.get(), wrbuf_->length(), ec);
            if (ec) {
                io_.reset();
                return Poll<folly::Unit>(IOError("pollComplete", ec));
            } else if (len == 0) {
                if (io_->poll_write().isReady())
                    continue;
                return Poll<folly::Unit>(not_ready);
            } else {
                wrbuf_->trimStart(len);
            }
        }
        assert(wrbuf_->length() == 0);
        wrbuf_->retreat(wrbuf_->headroom());
        return makePollReady(folly::Unit());
    }

private:
    std::unique_ptr<Io> io_;
    CODEC codec_;
    std::unique_ptr<folly::IOBuf> wrbuf_;
};

class DescriptorIo : public io::Io, public EventWatcherBase {
public:
    DescriptorIo(EventExecutor* reactor, int fd)
        : reactor_(reactor), rio_(reactor->getLoop()),
        wio_(reactor_->getLoop()), fd_(fd) {
        assert(reactor_);
        if (fd < 0) throw IOError("invalid fd");
        // reactor_->linkWatcher(this);
        rio_.set<DescriptorIo, &DescriptorIo::onEvent>(this);
        wio_.set<DescriptorIo, &DescriptorIo::onEvent>(this);
    }

    Async<folly::Unit> poll_read() override {
        assert(!event_hook_.is_linked());
        reactor_->linkWatcher(this);
        task_ = CurrentTask::park();
        rio_.start(fd_, ev::READ);
        return not_ready;
    }

    Async<folly::Unit> poll_write() override {
        assert(!event_hook_.is_linked());
        reactor_->linkWatcher(this);
        task_ = CurrentTask::park();
        wio_.start(fd_, ev::WRITE);
        return not_ready;
    }

    void cleanup(int reason) override {
        rio_.stop();
        wio_.stop();
        if (task_) task_->unpark();
    }

    virtual ~DescriptorIo() {
        clear();
    }

private:
    EventExecutor* reactor_;
    ev::io rio_;
    ev::io wio_;
    int fd_;
    Optional<Task> task_;

    void onEvent(ev::io &watcher, int revent) {
        if (revent & ev::ERROR)
            throw EventException("syscall error");
        FUTURES_DLOG(INFO) << "EVENT: " << fd_ << " " << task_.hasValue();
        if (event_hook_.is_linked())
            reactor_->unlinkWatcher(this);
        if (task_) task_->unpark();
        task_.clear();
    }

    void clear() {
        rio_.stop();
        wio_.stop();
        if (event_hook_.is_linked())
            reactor_->unlinkWatcher(this);
        task_.clear();
    }

};

class SendFuture : public FutureBase<SendFuture, ssize_t> {
public:
    enum State {
        INIT,
        SENT,
        CANCELLED,
    };
    using Item = ssize_t;

    SendFuture(std::unique_ptr<Io> io,
            std::unique_ptr<folly::IOBuf> buf)
        : io_(std::move(io)), s_(INIT),
        buf_(std::move(buf)) {}

    Poll<Item> poll() override {
    retry:
        std::error_code ec;
        switch (s_) {
            case INIT: {
                // register event
                ssize_t len = io_->write(*buf_.get(), buf_->length(), ec);
                if (ec) {
                    io_.reset();
                    return Poll<Item>(IOError("send", ec));
                } else if (len == 0) {
                    if (io_->poll_write().isNotReady()) {
                        s_ = INIT;
                    } else {
                        goto retry;
                    }
                } else {
                    s_ = SENT;
                    io_.reset();
                    return makePollReady(len);
                }
                break;
            }
            case CANCELLED:
                return Poll<Item>(FutureCancelledException());
            default:
                throw InvalidPollStateException();
        }

        return Poll<Item>(not_ready);
    }

    void cancel() override {
        io_.reset();
        s_ = CANCELLED;
    }
private:
    std::unique_ptr<Io> io_;
    State s_;
    std::unique_ptr<folly::IOBuf> buf_;
};

class TransferAtLeast {
public:
    TransferAtLeast(ssize_t length, ssize_t buf_size)
        : length_(length), buf_size_(buf_size) {
        assert(length > 0);
        assert(buf_size >= length);
    }

    TransferAtLeast(ssize_t length)
        : length_(length), buf_size_(length * 2) {
        assert(length >= 0);
    }

    size_t bufferSize() const {
        return buf_size_;
    }

    size_t remainBufferSize() const {
        assert(buf_size_ >= read_);
        return buf_size_ - read_;
    }

    // mark readed
    bool read(ssize_t s) {
        assert(s >= 0);
        read_ += s;
        if (read_ >= length_)
            return true;
        return false;
    }

private:
    const ssize_t length_;
    const ssize_t buf_size_;
    ssize_t read_ = 0;
};

class TransferExactly : public TransferAtLeast {
public:
    TransferExactly(ssize_t size)
        : TransferAtLeast(size, size) {}
};

template <typename ReadPolicy>
class RecvFuture : public FutureBase<RecvFuture<ReadPolicy>,
    std::unique_ptr<folly::IOBuf>>
{
public:
    enum State {
        INIT,
        DONE,
        CANCELLED,
    };
    using Item = std::unique_ptr<folly::IOBuf>;

    RecvFuture(std::unique_ptr<Io> io, const ReadPolicy &policy)
        : policy_(policy), io_(std::move(io)),
        buf_(folly::IOBuf::create(policy_.bufferSize())) {
    }

    Poll<Item> poll() override {
    retry:
        switch (s_) {
            case INIT: {
                // register event
                std::error_code ec;
                ssize_t len = io_->read(buf_.get(), policy_.remainBufferSize(), ec);
                FUTURES_DLOG(INFO) << "S " << "LEN " << len;
                if (ec) {
                    return Poll<Item>(IOError("recv", ec));
                } else if (len == 0) {
                    s_ = INIT;
                } else {
                    buf_->append(len);
                    if (policy_.read(len)) {
                        s_ = DONE;
                        io_.reset();
                        return Poll<Item>(Async<Item>(std::move(buf_)));
                    }
                }
                if (io_->poll_read().isNotReady()) {
                    break;
                } else {
                    goto retry;
                }
                break;
            }
            case CANCELLED:
                return Poll<Item>(FutureCancelledException());
            default:
                throw InvalidPollStateException();
        }

        return Poll<Item>(not_ready);

    }

    void cancel() override {
        io_.reset();
        s_ = CANCELLED;
    }
private:
    State s_ = INIT;
    ReadPolicy policy_;
    std::unique_ptr<Io> io_;
    std::unique_ptr<folly::IOBuf> buf_;
};

}
}