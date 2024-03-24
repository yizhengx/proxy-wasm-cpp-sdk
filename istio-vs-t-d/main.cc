#include <string>
#include <unordered_map>
#include <queue>
#include <chrono>
#include <cstdint>
#include "proxy_wasm_intrinsics.h"
#include <map>
#include "json.hpp"
using json = nlohmann::json;

namespace {
// tooManyRequest returns a 429 response code.
void tooManyRequest() {
  sendLocalResponse(429, "Too many requests", "rate_limited", {});
}
}

constexpr char sharedKey[] =
    "wasm_custom_logic.last_timeout";

struct QueueElement{
  uint32_t contextID;
  std::chrono::microseconds timeout;
};

class ExampleRootContext : public RootContext {
public:
    explicit ExampleRootContext(uint32_t id, std::string_view root_id) : RootContext(id, root_id) {}
    bool onConfigure(size_t) override;
    void onTick() override;
    void pushRequest(uint32_t, std::chrono::microseconds);
    bool isQueueFull();
    uint64_t getDelay();
    uint64_t getProcessingTime();
private:
    uint64_t queue_size;  
    uint64_t processing_time;
    uint64_t delay;
    uint64_t timer_tick;
    std::queue<QueueElement> queue;  
    bool parseConfiguration(size_t);
    bool initializeSharedData();
};

uint64_t ExampleRootContext::getDelay(){
    return delay;
};

uint64_t ExampleRootContext::getProcessingTime(){
    return processing_time;
};

bool ExampleRootContext::isQueueFull(){
    return queue.size() >= queue_size;
};

void ExampleRootContext::pushRequest(uint32_t contextID, std::chrono::microseconds timeout){
    queue.push(QueueElement{
        contextID, timeout
    });
    // LOG_WARN("Push request with ID "+std::to_string(contextID));
}

bool ExampleRootContext::onConfigure(size_t configuration_size) {
    if (!parseConfiguration(configuration_size)) {
        return false;
    }

    if (!initializeSharedData()){
        return false;
    }

    // Start ticker, which will triger request resumpton.
    proxy_set_tick_period_milliseconds(timer_tick);

    queue_size = 1024;

    return true;
};

bool ExampleRootContext::initializeSharedData(){
    // Check if the shared timeout is already initialized.
    WasmDataPtr last_timeout;
    if (WasmResult::Ok ==
        getSharedData(sharedKey, &last_timeout)) {
        return true;
    }
    // If not yet initialized, set last timeout to UINT64_MAX
    std::chrono::microseconds init_timeout = std::chrono::microseconds(0);
    auto res = setSharedData(sharedKey,
                            {reinterpret_cast<const char *>(&init_timeout),
                                sizeof(init_timeout)});
    if (res != WasmResult::Ok) {
        LOG_WARN("failed to initialize local rate limit last timeout");
        return false;
    }
    return true;
};

bool ExampleRootContext::parseConfiguration(size_t configuration_size) {
    auto configuration_data = getBufferBytes(WasmBufferType::PluginConfiguration,
                                           0, configuration_size);
    std::string result = configuration_data->toString();
    // set default config
    delay=500;
    timer_tick=1;
    // '{"delay": "${delay}", "processing_time": "${tick}"}'
    json jsonData = json::parse(result);
    // Access fields
    std::string delay_str = jsonData["delay"];
    std::string processing_time_str = jsonData["processing_time"];
    delay = std::stoi(delay_str, nullptr, 0);
    processing_time = std::stoi(processing_time_str, nullptr, 0);
    LOG_WARN("Set delay to "+ std::to_string(delay));
    LOG_WARN("Set processing_time to "+ std::to_string(processing_time));
    return true;
};

void ExampleRootContext::onTick(){
    std::chrono::microseconds now = std::chrono::microseconds(1);
    while (!queue.empty())
    {
        QueueElement ele = queue.front();
        now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
        if (ele.timeout < now){
            proxy_set_effective_context(ele.contextID);
            continueRequest();
            queue.pop();
            // LOG_WARN("Resume request with ID "+std::to_string(ele.contextID)+" passing id + wrapped continueRequest");
        }else{
            break;
        }
    }
    
};

class ExampleContext : public Context {
 public:
  explicit ExampleContext(uint32_t id, RootContext* root) : Context(id, root) {}
  FilterHeadersStatus onRequestHeaders(uint32_t, bool) override;

 private:
  inline ExampleRootContext* rootContext() {
    return dynamic_cast<ExampleRootContext*>(this->root());
  }
  std::chrono::microseconds getTimeout();
};

std::chrono::microseconds ExampleContext::getTimeout(){
    // LOG_WARN("Start getting timeout for request with ID "+std::to_string(id()));
    WasmDataPtr last_timeout;
    uint32_t cas;
    WasmResult get_res = getSharedData(sharedKey, &last_timeout, &cas);
    while (get_res != WasmResult::Ok){
        get_res = getSharedData(sharedKey, &last_timeout, &cas);
    };
    std::chrono::microseconds val =
        *reinterpret_cast<const std::chrono::microseconds *>(last_timeout->data());
    std::chrono::microseconds timeout;
    if (val <= std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())){
        // last timeout <= cur_time => queue is empty
        timeout = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()) 
            + std::chrono::microseconds(rootContext()->getProcessingTime());
    } else {
        timeout = val + std::chrono::microseconds(rootContext()->getDelay()) + std::chrono::microseconds(rootContext()->getProcessingTime());
    }
    auto res = setSharedData(
        sharedKey,
        {reinterpret_cast<const char *>(&timeout), sizeof(timeout)}, cas);
    while (res != WasmResult::Ok) {
        // updated unsuccesful, retry
        get_res = getSharedData(sharedKey, &last_timeout, &cas);
        while (get_res != WasmResult::Ok){
            get_res = getSharedData(sharedKey, &last_timeout, &cas);
        };
        val = *reinterpret_cast<const std::chrono::microseconds *>(last_timeout->data());
        if (val <= std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())){
            // last timeout <= cur_time => queue is empty
            timeout = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()) 
                + std::chrono::microseconds(rootContext()->getProcessingTime());
        } else {
            timeout = val + std::chrono::microseconds(rootContext()->getDelay()) + std::chrono::microseconds(rootContext()->getProcessingTime());
        }
        res = setSharedData(
            sharedKey,
            {reinterpret_cast<const char *>(&timeout), sizeof(timeout)}, cas);
    }
    return timeout;
};

FilterHeadersStatus ExampleContext::onRequestHeaders(uint32_t, bool) {
    // LOG_WARN("Receive request with ID "+std::to_string(id()));
    if (rootContext()->isQueueFull()){
        LOG_WARN("Queue is full, deny request with ID "+std::to_string(id()));
        tooManyRequest();
        return FilterHeadersStatus::StopIteration;
    }
    std::chrono::microseconds timeout = getTimeout();
    rootContext()->pushRequest(id(), timeout);
    return FilterHeadersStatus::StopAllIterationAndBuffer;
};

static RegisterContextFactory register_ExampleContext(CONTEXT_FACTORY(ExampleContext),
                                                      ROOT_FACTORY(ExampleRootContext),
                                                      "dcoz_root_id");