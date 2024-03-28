#include <string>
#include <unordered_map>
#include <queue>
#include <chrono>
#include <cstdint>
#include "proxy_wasm_intrinsics.h"
#include <map>

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
    uint64_t incoming_counter;
    uint64_t outgoing_counter;
    std::chrono::microseconds last_print_time;
private:
    uint64_t queue_size;  
    uint64_t delay;
    uint64_t timer_tick;
    std::queue<QueueElement> queue;  
    bool parseConfiguration(size_t);
    bool initializeSharedData();
};

uint64_t ExampleRootContext::getDelay(){
    return delay;
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
    logWarn("onConfigure - istio-original-vs-with-counting-per-thread");
    if (!parseConfiguration(configuration_size)) {
        return false;
    }

    if (!initializeSharedData()){
        return false;
    }

    // Start ticker, which will triger request resumpton.
    proxy_set_tick_period_milliseconds(timer_tick);

    queue_size = 1024;

    // logging
    incoming_counter = 0;
    outgoing_counter = 0;
    last_print_time = std::chrono::microseconds(0);
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
    std::string first_arg = result.substr(1, result.find(",")-1);
    std::string delay_str = first_arg.substr(first_arg.find(":")+1, first_arg.length()-first_arg.find(":"));
    delay = std::stoi(delay_str.substr(2, delay_str.length()-3), nullptr, 0);
    LOG_WARN("Set delay to "+ std::to_string(delay));
    return true;
};

void ExampleRootContext::onTick(){
    std::chrono::microseconds now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
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
    if (now.count()>last_print_time.count()+1000000){
        if (incoming_counter!=0 || outgoing_counter!=0){
            LOG_WARN("Time ["+std::to_string(now.count())+"]: incoming_counter="+std::to_string(incoming_counter)+" && outgoing_counter"+std::to_string(outgoing_counter));
        }
        last_print_time = now;
        incoming_counter = 0;
        outgoing_counter = 0;
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
    std::chrono::microseconds timeout = max(val, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()))
        + std::chrono::microseconds(rootContext()->getDelay());
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
        timeout = max(val, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()))
            + std::chrono::microseconds(rootContext()->getDelay());
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