#include "icom/core/loop_thread.hpp"

namespace icom::core {

LoopThread::LoopThread()
    : thread_([this](std::stop_token token) { loop_.run(token); }) {}

// jthread's own destructor already requests_stop() (which EventLoop::run()
// observes via the std::stop_callback it installs, and wakes for) then
// joins -- nothing left to do here beyond running loop_'s own destructor
// afterward, which member destruction order (reverse of declaration, so
// thread_ before loop_) already guarantees happens only once the
// background thread has actually stopped touching loop_.
LoopThread::~LoopThread() = default;

} // namespace icom::core
