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

#include <k2/cpo/client/Client.h>
#include <k2/module/k23si/client/k23si_client.h>
using namespace k2;

namespace k2::log {
inline thread_local k2::logging::Logger ptest("k2::ptest");
}
const char* collname = "k23si_test_collection";

// Integration tests for partition operations
// These assume three partitions with range partition schema: ["", c), [c, e), [e, "")
class PartitionTest {

public:  // application lifespan
    PartitionTest() : _client(k2::K23SIClientConfig()) { K2LOG_I(log::ptest, "ctor");}
    ~PartitionTest(){ K2LOG_I(log::ptest, "dtor");}

    seastar::future<dto::Timestamp> getTimeNow() {
        return AppBase().getDist<tso::TSOClient>().local().getTimestamp();
    }

    // required for seastar::distributed interface
    seastar::future<> gracefulStop() {
        K2LOG_I(log::ptest, "stop");
        return std::move(_testFuture);
    }

    seastar::future<> start(){
        K2LOG_I(log::ptest, "start");

        _cpoEndpoint = k2::RPC().getTXEndpoint(_cpoConfigEp());
        _testFuture = seastar::make_ready_future()
        .then([this] {
            K2LOG_I(log::ptest, "Getting the timestamp...");
            return getTimeNow();
        })
        .then([this] (dto::Timestamp&&) {
            return _client.start();
        })
        .then([this] {
            K2LOG_I(log::ptest, "Creating test collection...");
            std::vector<k2::String> rangeEnds;
            rangeEnds.push_back("c");
            rangeEnds.push_back("e");
            rangeEnds.push_back("");
            k2::dto::CollectionMetadata meta{
                .name = collname,
                .hashScheme = dto::HashScheme::Range,
                .storageDriver = dto::StorageDriver::K23SI,
                .capacity{
                    .dataCapacityMegaBytes = 0,
                    .readIOPs = 0,
                    .writeIOPs = 0,
                    .minNodes = 3
                },
                .retentionPeriod = 5h
            };
            return _client.makeCollection(std::move(meta), std::move(rangeEnds));
        })
        .then([](auto&& status) {
            K2EXPECT(log::ptest, status.is2xxOK(), true);
        })
        .then([this] { return runScenario01(); })
        .then([this] {
            K2LOG_I(log::ptest, "======= All tests passed ========");
            exitcode = 0;
        })
        .handle_exception([this](auto exc) {
            try {
                std::rethrow_exception(exc);
            } catch (std::exception& e) {
                K2LOG_E(log::ptest, "======= Test failed with exception [{}] ========", e.what());
                exitcode = -1;
            } catch (...) {
                K2LOG_E(log::ptest, "Test failed with unknown exception");
                exitcode = -1;
            }
        })
        .finally([this] {
            K2LOG_I(log::ptest, "======= Test ended ========");
            AppBase().stop(exitcode);
        });

        return seastar::make_ready_future();
    }

private:
    int exitcode = -1;
    k2::ConfigVar<k2::String> _cpoConfigEp{"cpo"};
    seastar::future<> _testFuture = seastar::make_ready_future();

    k2::K23SIClient _client;
    k2::dto::PartitionGetter _pgetter;
    std::unique_ptr<k2::TXEndpoint> _cpoEndpoint;


public: // tests

// get partition for key through range scheme
seastar::future<> runScenario01() {
    K2LOG_I(log::ptest, "runScenario01");

    return seastar::make_ready_future()
    .then([this] {
        // get partitions
        auto request = k2::dto::CollectionGetRequest{.name = collname};
        return k2::RPC().callRPC<k2::dto::CollectionGetRequest, k2::dto::CollectionGetResponse>
                (k2::dto::Verbs::CPO_COLLECTION_GET, request, *_cpoEndpoint, 100ms);
    })
    .then([this](auto&& response) {
        // check and assign it to _pgetter
        auto&[status, resp] = response;
        K2LOG_D(log::ptest, "get collection status: {}", status.code);
        K2EXPECT(log::ptest, status, k2::dto::K23SIStatus::OK);
        _pgetter = k2::dto::PartitionGetter(std::move(resp.collection));
    })
    .then([this] {
        K2LOG_I(log::ptest, "case1: get Partition for key with default reverse and exclusiveKey flag");

        k2::dto::Key key{.schemaName = "schema", .partitionKey = "d", .rangeKey = ""};
        k2::dto::PartitionGetter::PartitionWithEndpoint& part = _pgetter.getPartitionForKey(key);
        K2LOG_D(log::ptest, "partition: {}", *part.partition);
        K2EXPECT(log::ptest, part.partition->keyRangeV.startKey, "c");
    })
    .then([this] {
        K2LOG_I(log::ptest, "case2: using an empty key to get Partition with default reverse and exclusiveKey flag");

        k2::dto::Key key{.schemaName = "schema", .partitionKey = "", .rangeKey = ""};
        k2::dto::PartitionGetter::PartitionWithEndpoint& part = _pgetter.getPartitionForKey(key);
        K2LOG_D(log::ptest, "partition: {}", *part.partition);
        K2EXPECT(log::ptest, part.partition->keyRangeV.startKey, "");
    })
    .then([this] {
        K2LOG_I(log::ptest, "case3: get Partition for key with reverse flag set to be TRUE and default exclusiveKey flag");

        k2::dto::Key key{.schemaName = "schema", .partitionKey = "c", .rangeKey = ""};
        k2::dto::PartitionGetter::PartitionWithEndpoint& part = _pgetter.getPartitionForKey(key, true);
        K2LOG_D(log::ptest, "partition: {}", *part.partition);
        K2EXPECT(log::ptest, part.partition->keyRangeV.startKey, "c");
    })
    .then([this] {
        K2LOG_I(log::ptest, "case4: get Partition for EMPTY key with reverse flag set to be TRUE and default exclusiveKey flag");

        k2::dto::Key key{.schemaName = "schema", .partitionKey = "", .rangeKey = ""};
        k2::dto::PartitionGetter::PartitionWithEndpoint& part = _pgetter.getPartitionForKey(key, true);
        K2LOG_D(log::ptest, "partition: {}", *part.partition);
        K2EXPECT(log::ptest, part.partition->keyRangeV.startKey, "e");
    })
    .then([this] {
        K2LOG_I(log::ptest, "case5: get Partition for key with reverse and exclusiveKey flag set to be TRUE. the key is NOT the Start key of any partitions.");

        k2::dto::Key key{.schemaName = "schema", .partitionKey = "a", .rangeKey = ""};
        k2::dto::PartitionGetter::PartitionWithEndpoint& part = _pgetter.getPartitionForKey(key, true, true);
        K2LOG_D(log::ptest, "partition: {}", *part.partition);
        K2EXPECT(log::ptest, part.partition->keyRangeV.startKey, "");
    })
    .then([this] {
        K2LOG_I(log::ptest, "case6: get Partition for key with reverse and exclusiveKey flag set to be TRUE. the key is the Start key of a partition.");

        k2::dto::Key key{.schemaName = "schema", .partitionKey = "e", .rangeKey = ""};
        k2::dto::PartitionGetter::PartitionWithEndpoint& part = _pgetter.getPartitionForKey(key, true, true);
        K2LOG_D(log::ptest, "partition: {}", *part.partition);
        K2EXPECT(log::ptest, part.partition->keyRangeV.startKey, "c");
    })
    .then([this] {
        K2LOG_I(log::ptest, "case7: using an empty key to get Partition with reverse and exclusiveKey flag set to be TRUE.");

        k2::dto::Key key{.schemaName = "schema", .partitionKey = "", .rangeKey = ""};
        k2::dto::PartitionGetter::PartitionWithEndpoint& part = _pgetter.getPartitionForKey(key, true, true);
        K2LOG_D(log::ptest, "partition: {}", *part.partition);
        K2EXPECT(log::ptest, part.partition->keyRangeV.startKey, "e");
    });
}

};

int main(int argc, char** argv) {
    k2::App app("PartitionTest");
    app.addOptions()
        ("cpo", bpo::value<k2::String>(), "URL of Control Plane Oracle (CPO), e.g. 'tcp+k2rpc://192.168.1.2:12345'");
    app.addApplet<k2::tso::TSOClient>();
    app.addApplet<PartitionTest>();
    return app.start(argc, argv);
}

