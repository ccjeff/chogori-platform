/*
MIT License

Copyright(c) 2020 Futurewei Cloud

    Permission is hereby granted,
    free of charge, to any person obtaining a copy of this software and associated documentation files(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions :

    The above copyright notice and this permission notice shall be included in all copies
    or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS",
    WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER
    LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

// stl
#include <k2/appbase/Appbase.h>
#include <k2/appbase/AppEssentials.h>
#include <k2/transport/RetryStrategy.h>
#include "Log.h"
#include "txbench_common.h"
using namespace k2;

class Client {
public:  // application lifespan
    Client():
        _tcpRemotes(Config()["tcp_remotes"].as<std::vector<String>>()),
        _testDuration(Config()["test_duration_s"].as<uint32_t>()*1s),
        _data(0),
        _stopped(true),
        _haveSendPromise(false),
        _timer(seastar::timer<>([this] {
            _stopped = true;
            if (_haveSendPromise) {
                _haveSendPromise = false;
                _sendProm.set_value();
            }
        })),
        _lastAckedTotal(0) {
        _session.config = SessionConfig{.echoMode=Config()["echo_mode"].as<bool>(),
                                        .responseSize=Config()["request_size"].as<uint32_t>(),
                                        .pipelineSize=Config()["pipeline_depth_mbytes"].as<uint32_t>() * 1024 * 1024,
                                        .pipelineCount=Config()["pipeline_depth_count"].as<uint32_t>(),
                                        .ackCount=Config()["ack_count"].as<uint32_t>()};
        _data = (char*)malloc(_session.config.responseSize);
        K2LOG_I(log::txbench, "ctor");
    };

    ~Client() {
        K2LOG_I(log::txbench, "dtor");
        free(_data);
    }

    // required for seastar::distributed interface
    seastar::future<> gracefulStop() {
        K2LOG_I(log::txbench, "stop");
        if (_stopped) {
            return seastar::make_ready_future<>();
        }
        _stopped = true;
        // unregister all observers
        RPC().registerMessageObserver(ACK, nullptr);
        RPC().registerLowTransportMemoryObserver(nullptr);
        if (_haveSendPromise) {
            _sendProm.set_value();
        }
        return _stopPromise.get_future();
    }

    void registerMetrics() {
        _metric_groups.clear();
        std::vector<sm::label_instance> labels;
        labels.push_back(sm::label_instance("total_cores", seastar::smp::count));
        labels.push_back(sm::label_instance("active_cores", std::min(_tcpRemotes.size(), size_t(seastar::smp::count))));
        _metric_groups.add_group("session", {
            sm::make_gauge("ack_batch_size", _session.config.ackCount, sm::description("How many messages we ack at once"), labels),
            sm::make_gauge("session_id", _session.sessionID, sm::description("Session ID"), labels),
            sm::make_counter("total_count", _session.totalCount, sm::description("Total number of requests"), labels),
            sm::make_counter("total_bytes", _session.totalSize, sm::description("Total data bytes sent"), labels),
            sm::make_gauge("pipeline_depth", [this]{ return _session.config.pipelineCount  - _session.unackedCount;},
                    sm::description("Available pipeline depth"), labels),
            sm::make_gauge("pipeline_bytes", [this]{ return _session.config.pipelineSize  - _session.unackedSize;},
                    sm::description("Available pipeline bytes"), labels),
            sm::make_histogram("request_latency", [this]{ return _requestLatency.getHistogram();},
                    sm::description("Latency of acks"), labels)
        });
    }

    seastar::future<> start() {
        _stopped = false;

        return _discovery().then([this](){
            if (_stopped) return seastar::make_ready_future<>();
            K2LOG_I(log::txbench, "Setup complete. Starting session...");
            return _startSession();
        }).
        then([this]() {
            if (_stopped) return seastar::make_ready_future<>();
            K2LOG_I(log::txbench, "Setup complete. Starting benchmark in session: {}", _session.sessionID);
            return _benchmark();
        }).
        handle_exception([this](auto exc) {
            K2LOG_W_EXC(log::txbench, exc, "Unable to execute benchmark", exc);
            _stopped = true;
            return seastar::make_ready_future<>();
        }).finally([this]() {
            K2LOG_I(log::txbench, "Done with benchmark");
            auto totalsecs = ((double)msec(_actualTestDuration).count())/1000.0;
            auto szpsec = (((double)_session.totalSize - _session.unackedSize )/(1024*1024*1024))/totalsecs;
            auto cntpsec = ((double)_session.totalCount - _session.unackedCount)/totalsecs;
            K2LOG_I(log::txbench, "sessionID={}", _session.sessionID);
            K2LOG_I(log::txbench, "remote={}", _session.client.url);
            K2LOG_I(log::txbench, "totalSize={} ({} GBit per sec)", _session.totalSize, szpsec*8);
            K2LOG_I(log::txbench, "totalCount={}, ({} per sec)", _session.totalCount, cntpsec);
            K2LOG_I(log::txbench, "unackedSize={}", _session.unackedSize);
            K2LOG_I(log::txbench, "unackedCount={}", _session.unackedCount);
            K2LOG_I(log::txbench, "testDuration={}ms", msec(_actualTestDuration).count());

            _stopped = true;
            _stopPromise.set_value();
        });
    }

private:
    seastar::future<> _discovery() {
        auto myID = seastar::this_shard_id();
        K2LOG_I(log::txbench, "performing service discovery on core {}", myID);
        if (myID >= _tcpRemotes.size()) {
            K2LOG_W(log::txbench, "No TCP remote endpoint defined for core {}", myID);
            return seastar::make_exception_future<>(std::runtime_error("No remote endpoint defined"));
        }
        auto myRemote = RPC().getTXEndpoint(_tcpRemotes[myID]);
        auto retryStrategy = seastar::make_lw_shared<ExponentialBackoffStrategy>();
        retryStrategy->withRetries(10).withStartTimeout(10ms).withRate(3);
        return retryStrategy->run([this, myRemote=std::move(myRemote)](size_t, Duration timeout) {
            if (_stopped) {
                K2LOG_I(log::txbench, "Stopping retry since we were stopped");
                return seastar::make_exception_future<>(std::runtime_error("we were stopped"));
            }
            return RPC().sendRequest(GET_DATA_URL, myRemote->newPayload(), *myRemote, timeout)
            .then([this](std::unique_ptr<Payload>&& payload) {
                if (_stopped) return seastar::make_ready_future<>();
                if (!payload || payload->getSize() == 0) {
                    K2LOG_E(log::txbench, "Remote end did not provide a data endpoint. Giving up");
                    return seastar::make_exception_future<>(std::runtime_error("no remote endpoint"));
                }
                String remoteURL;
                for (auto&& buf: payload->release()) {
                    remoteURL.append((char*)buf.get_write(), buf.size());
                }
                K2LOG_I(log::txbench, "Found remote data endpoint: {}", remoteURL);
                _session.client = *(RPC().getTXEndpoint(remoteURL));
                return seastar::make_ready_future<>();
            })
            .then_wrapped([this](auto&& fut) {
                if (_stopped) {
                    fut.ignore_ready_future();
                    return seastar::make_ready_future<>();
                }
                return std::move(fut);
            });
        })
        .finally([retryStrategy](){
            K2LOG_I(log::txbench, "Finished getting remote data endpoint");
        });
    }

    seastar::future<> _startSession() {
        _requestIssueTimes.clear();
        _requestIssueTimes.resize(_session.config.pipelineCount);
        _lastAckedTotal = 0;
        registerMetrics();

        auto req = _session.client.newPayload();
        req->write((void*)&_session.config, sizeof(_session.config));
        return RPC().sendRequest(START_SESSION, std::move(req), _session.client, 1s).
        then([this](std::unique_ptr<Payload>&& payload) {
            if (_stopped) return seastar::make_ready_future<>();
            if (!payload || payload->getSize() == 0) {
                K2LOG_E(log::txbench, "Remote end did not start a session. Giving up");
                return seastar::make_exception_future<>(std::runtime_error("no remote session"));
            }
            SessionAck ack{};
            payload->read((void*)&ack, sizeof(ack));
            _session.sessionID = ack.sessionID;
            K2LOG_I(log::txbench, "Starting session id={}", _session.sessionID);
            return seastar::make_ready_future<>();
        }).
        handle_exception([](auto exc) {
            K2LOG_W_EXC(log::txbench, exc, "Unable to start session");
            return seastar::make_exception_future<>(exc);
        });
    }

    seastar::future<> _benchmark() {
        K2LOG_I(log::txbench, "Starting benchmark for remote={}, config={}", _session.client.url, _session.config);

        RPC().registerMessageObserver(ACK, [this](Request&& request) {
            auto now = Clock::now(); // to compute reqest latencies
            if (request.payload) {
                Ack ack{};
                request.payload->read((void*)&ack, sizeof(ack));
                if (ack.sessionID != _session.sessionID) {
                    K2LOG_W(log::txbench, "Received ack for unknown session: have={}, recv={}",
                        _session.sessionID, ack.sessionID);
                    return;
                }
                if (ack.totalCount > _session.totalCount) {
                    K2LOG_W(log::txbench, "Received ack for too many messages: have={}, recv={}",
                         _session.totalCount, ack.totalCount);
                    return;
                }
                if (ack.totalCount <= _lastAckedTotal) {
                    K2LOG_W(log::txbench, "Received ack that is too old tc={}, uc={}, ac={}",
                        _session.totalCount, _session.unackedCount, ack.totalCount);
                }
                if (ack.totalSize > _session.totalSize) {
                    K2LOG_W(log::txbench, "Received ack for too much data: have={}, recv={}",
                         _session.totalSize, ack.totalSize);
                    return;
                }
                if (ack.checksum != (ack.totalCount*(ack.totalCount+1)) /2) {
                    K2LOG_W(log::txbench, "Checksum mismatch. got={}, exp={}", ack.checksum, (ack.totalCount*(ack.totalCount+1)) /2);
                }
                for (uint64_t reqid = _session.totalCount - _session.unackedCount; reqid < ack.totalCount; reqid++) {
                    auto idx = reqid % _session.config.pipelineCount;
                    auto dur = now - _requestIssueTimes[idx];
                    _requestLatency.add(dur);
                }
                _session.unackedCount = _session.totalCount - ack.totalCount;
                _session.unackedSize = _session.totalSize - ack.totalSize;
                if (_haveSendPromise) {
                    _haveSendPromise = false;
                    _sendProm.set_value();
                }
            }
        });

        _timer.arm(_testDuration);

        _start = Clock::now();
        return seastar::do_until(
            [this] { return _stopped; },
            [this] { // body of loop
                if (canSend()) {
                    return send();
                }
                else {
                    K2ASSERT(log::txbench, !_haveSendPromise, "no send promise");
                    _haveSendPromise = true;
                    _sendProm = seastar::promise<>();
                    return _sendProm.get_future();
                }
                return seastar::make_ready_future<>();
            }
        ).finally([this] {
            _actualTestDuration = Clock::now() - _start;
        });
    }

    bool canSend() {
        return _session.unackedSize < _session.config.pipelineSize &&
               _session.unackedCount < _session.config.pipelineCount;
    }

    seastar::future<> send() {
        auto payload = _session.client.newPayload();
        auto padding = sizeof(_session.sessionID) + sizeof(_session.totalCount);
        K2ASSERT(log::txbench, padding < _session.config.responseSize, "invalid padding");
        _session.totalSize += _session.config.responseSize;
        _session.totalCount += 1;
        _session.unackedSize += _session.config.responseSize;
        _session.unackedCount += 1;

        payload->write((void*)&_session.sessionID, sizeof(_session.sessionID));
        payload->write((void*)&_session.totalCount, sizeof(_session.totalCount));
        payload->write(_data, _session.config.responseSize - padding );
        _requestIssueTimes[_session.totalCount % _session.config.pipelineCount] = Clock::now();
        return RPC().send(REQUEST, std::move(payload), _session.client);
    }

private:
    std::vector<String> _tcpRemotes;
    Duration _testDuration;
    Duration _actualTestDuration;
    char* _data;
    BenchSession _session;
    bool _stopped;
    seastar::promise<> _sendProm;
    seastar::promise<> _stopPromise;
    bool _haveSendPromise;
    TimePoint _start;
    seastar::timer<> _timer;
    sm::metric_groups _metric_groups;
    ExponentialHistogram _requestLatency;
    std::vector<TimePoint> _requestIssueTimes;
    uint64_t _lastAckedTotal;
}; // class Client

int main(int argc, char** argv) {
    App app("txbench_client");
    app.addApplet<Client>();
    app.addOptions()
        ("request_size", bpo::value<uint32_t>()->default_value(512), "How many bytes to send with each request")
        ("ack_count", bpo::value<uint32_t>()->default_value(5), "How many messages do we ack at once")
        ("pipeline_depth_mbytes", bpo::value<uint32_t>()->default_value(200), "How much data do we allow to go un-ACK-ed")
        ("pipeline_depth_count", bpo::value<uint32_t>()->default_value(10), "How many requests do we allow to go un-ACK-ed")
        ("echo_mode", bpo::value<bool>()->default_value(false), "Should we echo all data in requests when we ACK. ")
        ("tcp_remotes", bpo::value<std::vector<String>>()->multitoken()->default_value(std::vector<String>()), "A list(space-delimited) of TCP remote endpoints to assign to each core. e.g. 'tcp+k2rpc://192.168.1.2:12345'")
        ("test_duration_s", bpo::value<uint32_t>()->default_value(30), "How long in seconds to run");
    return app.start(argc, argv);
}
