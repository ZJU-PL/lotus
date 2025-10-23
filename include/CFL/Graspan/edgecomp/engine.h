#ifndef ENGINE_H
#define ENGINE_H

#include <ctime>

// Boost 1.66+ renamed io_service to io_context
#include <boost/version.hpp>
#if BOOST_VERSION >= 106600
#include <boost/asio/io_context.hpp>
namespace boost_asio_compat {
    using io_service = boost::asio::io_context;
}
#else
#include <boost/asio/io_service.hpp>
namespace boost_asio_compat {
    using io_service = boost::asio::io_service;
}
#endif

#include <boost/thread/thread.hpp>
#include <boost/bind.hpp>

#include "compute.h"
#include "repart.h"

#include "../datastructures/context.h"
#include "../datastructures/vertex.h"
#include "../datastructures/loadedvertexinterval.h"
#include "../datastructures/computationset.h"
#include "../datastructures/loader.h"
#include "../utilities/timer.h"

long run_computation(Context &context);

#endif
