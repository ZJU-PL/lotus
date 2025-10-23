#ifndef ENGINE_H
#define ENGINE_H

#include <ctime>

#include <boost/asio/io_service.hpp>
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
