#include <string>
#include <unordered_map>
#include <queue>
#include <chrono>
#include <cstdint>
#include "proxy_wasm_intrinsics.h"
#include <map>
#include <random>
#include<unistd.h>
#include<thread>


constexpr char incoming_counter_key[] =
    "wasm_custom_logic.incoming_counter_key";
constexpr char outgoing_counter_key[] =
    "wasm_custom_logic.outgoing_counter_key";
constexpr char logger_init_key[] =
    "wasm_custom_logic.logger_init_key";
std::thread::id thread_id = std::this_thread::get_id();

class ExampleRootContext : public RootContext {
public:
    explicit ExampleRootContext(uint32_t id, std::string_view root_id) : RootContext(id, root_id) {}
    bool onConfigure(size_t) override;
    void onTick() override;
private:
    bool initializeSharedData();
};

bool ExampleRootContext::onConfigure(size_t configuration_size) {
    logWarn("onConfigure");
    if (!initializeSharedData()){
        return false;
    }
    
    sleep(2);
    // Check if logger is initialized
    WasmDataPtr logger_init_ptr;
    if (WasmResult::Ok ==
        getSharedData(logger_init_key, &logger_init_ptr)) {
        return true;
    }
    std::thread::id logger_thread_id = *reinterpret_cast<const std::thread::id *>(logger_init_ptr->data());;
    if (logger_thread_id==thread_id){
        logWarn("set Tick");
        proxy_set_tick_period_milliseconds(1000);
    }

    // Start ticker, which will triger request resumpton.

    return true;
};

bool ExampleRootContext::initializeSharedData(){
    // init incoming counter
    WasmDataPtr counter_ptr;
    if (WasmResult::Ok ==
        getSharedData(incoming_counter_key, &counter_ptr)) {
        return true;
    }
    uint64_t init_couter = 0;
    auto res = setSharedData(incoming_counter_key,
                            {reinterpret_cast<const char *>(&init_couter),
                                sizeof(init_couter)});
    if (res != WasmResult::Ok) {
        LOG_WARN("failed to initialize incoming_counter");
        return false;
    }

    // init outgoing counter
    if (WasmResult::Ok ==
        getSharedData(outgoing_counter_key, &counter_ptr)) {
        return true;
    }
    res = setSharedData(outgoing_counter_key,
                            {reinterpret_cast<const char *>(&init_couter),
                                sizeof(init_couter)});
    if (res != WasmResult::Ok) {
        LOG_WARN("failed to initialize outgoing_counter");
        return false;
    }
    return true;

    // 
    WasmDataPtr logger_init_ptr;
    if (WasmResult::Ok ==
        getSharedData(logger_init_key, &logger_init_ptr)) {
        return true;
    }
    res = setSharedData(logger_init_key,
                            {reinterpret_cast<const char *>(&thread_id),
                                sizeof(thread_id)});
    if (res != WasmResult::Ok) {
        LOG_WARN("failed to initialize logger_init_key");
        return false;
    }
};


void ExampleRootContext::onTick(){
    logWarn("Ontick");
    std::chrono::microseconds now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    );

    // get incoming counter
    WasmDataPtr incoming_counter_ptr;
    uint32_t cas;
    WasmResult get_res = getSharedData(incoming_counter_key, &incoming_counter_ptr, &cas);
    while (get_res != WasmResult::Ok){
        get_res = getSharedData(incoming_counter_key, &incoming_counter_ptr, &cas);
    };
    uint64_t incoming_counter = *reinterpret_cast<const uint64_t *>(incoming_counter_ptr->data());

    // get outgoing counter
    WasmDataPtr outgoing_counter_ptr;
    get_res = getSharedData(outgoing_counter_key, &outgoing_counter_ptr, &cas);
    while (get_res != WasmResult::Ok){
        get_res = getSharedData(outgoing_counter_key, &outgoing_counter_ptr, &cas);
    };
    uint64_t outgoing_counter = *reinterpret_cast<const uint64_t *>(outgoing_counter_ptr->data());

    // log
    LOG_WARN("Time ["+std::to_string(now.count())+"]: incoming_counter="+std::to_string(incoming_counter)+" && outgoing_counter"+std::to_string(outgoing_counter));
};

class ExampleContext : public Context {
 public:
  explicit ExampleContext(uint32_t id, RootContext* root) : Context(id, root) {}
  FilterHeadersStatus onRequestHeaders(uint32_t, bool) override;
  FilterHeadersStatus onResponseHeaders(uint32_t, bool) override;

 private:
  inline ExampleRootContext* rootContext() {
    return dynamic_cast<ExampleRootContext*>(this->root());
  }
};

FilterHeadersStatus ExampleContext::onRequestHeaders(uint32_t, bool) {
    WasmDataPtr incoming_counter_ptr;
    uint32_t cas;
    WasmResult get_res = getSharedData(incoming_counter_key, &incoming_counter_ptr, &cas);
    while (get_res != WasmResult::Ok){
        get_res = getSharedData(incoming_counter_key, &incoming_counter_ptr, &cas);
    };
    uint64_t incoming_counter = *reinterpret_cast<const uint64_t *>(incoming_counter_ptr->data()) + 1;
    auto res = setSharedData(
        incoming_counter_key,
        {reinterpret_cast<const char *>(&incoming_counter), sizeof(incoming_counter)}, cas);
    while (res != WasmResult::Ok) {
        // updated unsuccesful, retry
        get_res = getSharedData(incoming_counter_key, &incoming_counter_ptr, &cas);
        while (get_res != WasmResult::Ok){
            get_res = getSharedData(incoming_counter_key, &incoming_counter_ptr, &cas);
        };
        incoming_counter = *reinterpret_cast<const uint64_t *>(incoming_counter_ptr->data()) + 1;
        res = setSharedData(
            incoming_counter_key,
            {reinterpret_cast<const char *>(&incoming_counter), sizeof(incoming_counter)}, cas);
    }
    return FilterHeadersStatus::Continue;
};

FilterHeadersStatus ExampleContext::onResponseHeaders(uint32_t, bool) {
    WasmDataPtr outgoing_counter_ptr;
    uint32_t cas;
    WasmResult get_res = getSharedData(outgoing_counter_key, &outgoing_counter_ptr, &cas);
    while (get_res != WasmResult::Ok){
        get_res = getSharedData(outgoing_counter_key, &outgoing_counter_ptr, &cas);
    };
    uint64_t incoming_counter = *reinterpret_cast<const uint64_t *>(outgoing_counter_ptr->data()) + 1;
    auto res = setSharedData(
        outgoing_counter_key,
        {reinterpret_cast<const char *>(&incoming_counter), sizeof(incoming_counter)}, cas);
    while (res != WasmResult::Ok) {
        // updated unsuccesful, retry
        get_res = getSharedData(outgoing_counter_key, &outgoing_counter_ptr, &cas);
        while (get_res != WasmResult::Ok){
            get_res = getSharedData(outgoing_counter_key, &outgoing_counter_ptr, &cas);
        };
        incoming_counter = *reinterpret_cast<const uint64_t *>(outgoing_counter_ptr->data()) + 1;
        res = setSharedData(
            outgoing_counter_key,
            {reinterpret_cast<const char *>(&incoming_counter), sizeof(incoming_counter)}, cas);
    }
    return FilterHeadersStatus::Continue;
};

static RegisterContextFactory register_ExampleContext(CONTEXT_FACTORY(ExampleContext),
                                                      ROOT_FACTORY(ExampleRootContext),
                                                      "dcoz_root_id");