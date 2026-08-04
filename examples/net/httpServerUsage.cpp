import cppm.net.http;
import cppm.core.logger;

#include <string>
#include <chrono>
#include <thread>
#include <iostream>

int main()
{
    http::Server server;

    server.route(http::HttpMethod::Get, "/", [](const http::HttpRequest&)
        {
            logger::log(logger::LogLevel::Info, "Received request for /");
            http::HttpResponse r{};
            r.statusCode = 200;
            r.body = "A Modern HTTP Server Implemented Using C++ Modules";
            r.setHeader("Content-Type", "text/plain; charset=utf-8");
            return r;
        });

    server.start(8080);
    logger::log(logger::LogLevel::Info, "Server running on :8080");
    logger::log(logger::LogLevel::Info, "Press Enter to stop...");

    std::string line;
    std::getline(std::cin, line);  // waits until you press Enter

    server.stop();
    logger::log(logger::LogLevel::Info, "Server stopped");
}