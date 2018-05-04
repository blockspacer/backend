#ifndef SYNCAIDE_SERVER_HTTP_H
#define SYNCAIDE_SERVER_HTTP_H

#include "server/websocket.h"
#include "server/response.h"
#include "server/helper.h"
#include "server/router.h"
#include "server/ssl_stream.h"

#include <boost/beast/core.hpp>
#include <boost/beast/core/file.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/logic/tribool.hpp>
#include <boost/filesystem.hpp>
#include <boost/variant.hpp>
#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <functional>
#include <string>

using namespace std;
extern map<string, vector<unsigned char>> resources;

namespace server {
    namespace asio = boost::asio;
    namespace ssl = boost::asio::ssl;
    namespace http = boost::beast::http;
    namespace websocket = boost::beast::websocket;
    namespace fs = boost::filesystem;
    using nlohmann::json;
    using boost::system::error_code;
    using boost::beast::http::request;
    using boost::beast::http::response;
    using boost::beast::http::string_body;
    using boost::beast::http::field;
    using boost::beast::http::status;
    using boost::beast::http::verb;
    using boost::beast::string_view;
    using boost::beast::flat_buffer;
    using boost::beast::string_param;
    using boost::asio::bind_executor;
    using boost::asio::steady_timer;
    using boost::asio::ssl::context;
    using boost::asio::io_context;
    using boost::asio::ip::tcp;
    using boost::asio::ip::address;
    using boost::asio::strand;
    using boost::asio::error::operation_aborted;
    using boost::variant;
    using boost::get;
    using boost::tribool;

    class Http : public enable_shared_from_this<Http> {
        using Socket = variant<tcp::socket, ssl_stream<tcp::socket>>;
        using request_type = request<string_body>;
        using response_type = response<string_body>;

        class queue {
            enum {
                limit = 8
            };

        public:
            struct work {
                virtual ~work() = default;

                virtual void operator()() = 0;
            };

            explicit queue(Http &self) : _self(self) {
                static_assert(limit > 0, "queue limit must be positive");
                _items.reserve(limit);
            }

            bool is_full() const {
                return _items.size() >= limit;
            }

            bool on_write() {
                BOOST_ASSERT(!_items.empty());
                auto const was_full = is_full();
                _items.erase(_items.begin());
                if (!_items.empty()) {
                    (*_items.front())();
                }
                return was_full;
            }

            template<bool isRequest, class Body, class Fields>
            void operator()(http::message<isRequest, Body, Fields> &&msg) {
                struct work_impl : work {
                    Http &_self;
                    http::message<isRequest, Body, Fields> _msg;

                    work_impl(
                        Http &self,
                        http::message<isRequest, Body, Fields> &&msg
                    ) : _self(self),
                        _msg(move(msg)) {}

                    void operator()() override {
                        if (_self._secured) {
                            http::async_write(
                                get<ssl_stream<tcp::socket>>(_self._socket),
                                _msg,
                                bind_executor(
                                    _self._strand,
                                    bind(
                                        &Http::on_write,
                                        _self.shared_from_this(),
                                        placeholders::_1,
                                        (bool) _msg.need_eof()
                                    )
                                )
                            );
                        } else {
                            http::async_write(
                                get<tcp::socket>(_self._socket),
                                _msg,
                                bind_executor(
                                    _self._strand,
                                    bind(
                                        &Http::on_write,
                                        _self.shared_from_this(),
                                        placeholders::_1,
                                        (bool) _msg.need_eof()
                                    )
                                )
                            );
                        }
                    }
                };

                _items.emplace_back(new work_impl(_self, move(msg)));
                if (_items.size() == 1) {
                    (*_items.front())();
                }
            }

        private:
            Http &_self;
            vector<unique_ptr<work>> _items;
        };

    private:
        request_type _req;
        flat_buffer _buffer;
        steady_timer _timer;
        bool _eof = false;
        context &_ctx;
        shared_ptr<Router> _router;
        queue _queue;

    protected:
        Socket _socket;
        strand<io_context::executor_type> _strand;
        tribool _secured;

    public:
        Http(
            tcp::socket socket,
            flat_buffer buffer,
            tribool secured,
            context &ctx,
            shared_ptr<Router> router
        );

        void run();

        void read();

        void eof();

        void timeout();

        void on_handshake(error_code code, size_t bytes_used);

        void on_timer(error_code code);

        void on_read(error_code code);

        void on_write(error_code code, bool close);

        void on_shutdown(error_code code);

        static response_type health(request_type &req);

        static response_type syncaide_js(request_type &req);

        static response_type syncaide_wasm(request_type &req);

        static response_type agent_uid(request_type &req, const string &uid);

#ifndef NDEBUG

        static response_type syncaide_html(request_type &req);

#endif //NDEBUG

    private:
        static Socket deduce_socket(
            tcp::socket socket,
            context &ctx,
            tribool secured
        );
    };
}

#endif //SYNCAIDE_SERVER_HTTP_H