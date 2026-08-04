import cppm.net.http;
import cppm.core.logger;
import cppm.core.json;

#include <string>
#include <iostream>

int main()
{
    http::Server server;

    // POST /echo-json
    server.route(http::HttpMethod::Post, "/echo-json", [](const http::HttpRequest& req)
        {
            http::HttpResponse res{};

            try
            {
                auto doc = json::parse(req.body);

                // Add/override a field
                if (doc.isObject())
                {
                    doc.asObject()["handledBy"] = json::Json("cppm server");
                }

                res.statusCode = 200;
                res.body = json::stringify(doc, 2); // pretty JSON
                res.setHeader("Content-Type", "application/json; charset=utf-8");
            }
            catch (const std::exception& ex)
            {
                logger::log(logger::LogLevel::Error, "JSON parse failed: ", ex.what());
                res.statusCode = 400;
                res.body = "{\"error\":\"invalid json\"}";
                res.setHeader("Content-Type", "application/json; charset=utf-8");
            }

            return res;
        });

    // default route
    server.setNotFoundHandler([](const http::HttpRequest& req)
        {
            http::HttpResponse res{};
            res.statusCode = 404;
            res.body = "{\"error\":\"not found\",\"path\":\"" + req.target + "\"}";
            res.setHeader("Content-Type", "application/json; charset=utf-8");
            return res;
        });

    server.start(8080);
    logger::log(logger::LogLevel::Info, "Server running on :8080");
    std::string line;
    std::getline(std::cin, line);
    server.stop();
}

//Quick test with curl :
//curl - i - X POST http ://localhost:8080/echo-json -H "Content-Type: application/json" -d "{\"name\":\"praveen\"}"
//You should get JSON back with "handledBy" : "cppm server".