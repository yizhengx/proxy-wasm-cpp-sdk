#include <string>
#include <unordered_map>
#include <queue>
#include <chrono>
#include <cstdint>
#include "proxy_wasm_intrinsics.h"
#include <map>
#include <random>
#include<unistd.h>


class ExampleRootContext : public RootContext {
public:
    explicit ExampleRootContext(uint32_t id, std::string_view root_id) : RootContext(id, root_id) {}
    bool onConfigure(size_t) override;
    void onTick() override;
    uint64_t incoming_counter;
    uint64_t outgoing_counter;
};

bool ExampleRootContext::onConfigure(size_t configuration_size) {
    logWarn("onConfigure");
    proxy_set_tick_period_milliseconds(1000);
    return true;
};


void ExampleRootContext::onTick(){
    // logWarn("Ontick");
    std::chrono::microseconds now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    );
    // log
    if (incoming_counter!=0 || outgoing_counter!=0){
        LOG_WARN("Time ["+std::to_string(now.count())+"]: incoming_counter="+std::to_string(incoming_counter)+" && outgoing_counter"+std::to_string(outgoing_counter));
    }
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
    this->rootContext()->incoming_counter += 1;
    return FilterHeadersStatus::Continue;
};

FilterHeadersStatus ExampleContext::onResponseHeaders(uint32_t, bool) {
    this->rootContext()->outgoing_counter += 1;
    return FilterHeadersStatus::Continue;
};

static RegisterContextFactory register_ExampleContext(CONTEXT_FACTORY(ExampleContext),
                                                      ROOT_FACTORY(ExampleRootContext),
                                                      "dcoz_root_id");