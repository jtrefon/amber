#include "bench/mock_server.h"

#include <memory>
#include <mutex>
#include <utility>

namespace bench {

namespace {

EndpointReply from_mock(const MockReply& mock) {
    EndpointReply reply;
    reply.content = mock.content;
    reply.tool_calls = mock.tool_calls;
    reply.prompt_tokens = mock.prompt_tokens;
    reply.completion_tokens = mock.completion_tokens;
    return reply;
}

} // namespace

MockServer::MockServer(std::deque<MockReply> script) {
    auto queue = std::make_shared<std::deque<MockReply>>(std::move(script));
    auto mutex = std::make_shared<std::mutex>();
    endpoint_ = std::make_unique<ScriptedEndpoint>([queue, mutex](const EndpointRequest& request) {
        if (request.path != "/v1/chat/completions") {
            EndpointReply reply;
            reply.handled = false;
            return reply;
        }
        EndpointReply reply;
        std::scoped_lock lock(*mutex);
        if (queue->empty()) {
            reply.content = "done";
            return reply;
        }
        reply = from_mock(queue->front());
        queue->pop_front();
        return reply;
    });
}

MockServer::~MockServer() {
    stop();
}

bool MockServer::start() {
    return endpoint_->start();
}

void MockServer::stop() {
    if (endpoint_)
        endpoint_->stop();
}

std::string MockServer::base_url() const {
    return endpoint_->base_url();
}

std::vector<std::string> MockServer::paths() const {
    return endpoint_->paths();
}

} // namespace bench
