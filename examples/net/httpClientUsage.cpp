import cppm.net.http;
import cppm.core.logger;

#include <iostream>
#include <string>
#include <exception>

int main()
{
	logger::setLevel(logger::LogLevel::Debug);

    logger::log(logger::LogLevel::Info,"Starting request");

    http::Client client;

    http::HttpOptions opts{};
    opts.timeoutMs = 5000;
    opts.maxRetries = 1;
    client.setDefaultOptions(opts);

    http::HttpRequest req{};
    req.method = http::HttpMethod::Get;
    req.target = "/";
    req.setHeader("User-Agent", "cppm-sample/1.0");

    try
    {
        // Sync call
        //http::HttpResponse res = client.send(req, "example.com", 80);
        http::HttpResponse res = client.send(req, "saucedemo.com", 80);

        if (res.ok())
        {
            logger::log(logger::LogLevel::Info, "HTTP success: ", res.statusCode);
			logger::log(logger::LogLevel::Debug, "Response body: ", res.body);
        }
        else
        {
            logger::log(logger::LogLevel::Warning, "HTTP status: ", res.statusCode);
        }

         //Async call example
         //http::HttpEndpoint ep{};
         //ep.scheme = http::HttpScheme::Http;
         //ep.host = "example.com";
         //ep.port = 80;
         //ep.target = "/";
         //auto fut = client.sendAsync(ep, req);
         //http::HttpResponse asyncRes = fut.get();
         //logger::Logger::instance().info("Async status: ", asyncRes.statusCode);
    }
    catch (const std::exception& ex)
    {
        logger::log(logger::LogLevel::Error, "Request failed: ", ex.what());
        logger::log(logger::LogLevel::Debug, "Client lastError: ", client.lastError());
    }

    return 0;
}