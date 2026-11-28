#pragma once

#include <cerrno>
#include <utility>

#include <zmq.hpp>

namespace thewatcher::zmqutil
{

// Calls operation() and calls it again whenever ZeroMQ reports EINTR. A signal
// that lands on a thread blocked in zmq_poll / zmq_recv / zmq_send makes cppzmq
// throw zmq::error_t with EINTR; that is not a failure of the socket, so the
// call is simply repeated. Any other zmq::error_t propagates unchanged. The
// operation must be safe to repeat: an interrupted ZeroMQ call has had no
// effect.
template <typename Operation>
decltype(auto) retry_on_eintr(Operation&& operation)
{
    while (true)
    {
        try
        {
            return std::forward<Operation>(operation)();
        }
        catch (const zmq::error_t& e)
        {
            if (e.num() != EINTR)
                throw;
        }
    }
}

} // namespace thewatcher::zmqutil
