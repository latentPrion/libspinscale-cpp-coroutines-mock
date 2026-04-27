#ifndef CURRENT_IO_CONTEXT_H
#define CURRENT_IO_CONTEXT_H

#include <boost/asio/io_context.hpp>

boost::asio::io_context &current_io_context();

#endif // CURRENT_IO_CONTEXT_H
