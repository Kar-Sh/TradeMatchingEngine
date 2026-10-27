#include <chrono>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "functions.hpp"


namespace
{
std::string utc_now()
{
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void write_snapshot(const MatchingEngine& engine, std::ofstream& file)
{
    std::string time = utc_now();
    for (const auto& s : engine.snapshot(3))
        file << format_snapshot(s, time) << '\n';
    file.flush();
}

class SnapshotWriter
{
public:
    SnapshotWriter(const MatchingEngine& engine, const std::string& path)
        : engine_(engine), file_(path, std::ios::trunc), thread_([this] { run(); })
    {
    }

    ~SnapshotWriter()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_)
        {
            // Wait for the next snapshot, unless shutdown was requested.
            if (!cv_.wait_for(lock, std::chrono::seconds(5), [this] { return stop_; }))
                write_snapshot(engine_, file_);
        }
        write_snapshot(engine_, file_);  // Save one last snapshot before exiting
    }

    const MatchingEngine& engine_;
    std::ofstream file_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};
}

int main()
{
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    try
    {
        auto logger = spdlog::basic_logger_mt("engine", "engine.log", true);
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
    }
    catch (const spdlog::spdlog_ex& e)
    {
        std::cerr << "Log init failed: " << e.what() << '\n';
    }

    MatchingEngine engine;
    SnapshotWriter telemetry(engine, "snapshots.log");

    std::string line;
    while (std::getline(std::cin, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos)
            continue;

        try
        {
            for (const auto& out : engine.handle_line(line))
                std::cout << out << '\n';
        }
        catch (const std::exception& e)
        {
            spdlog::critical("Unhandled exception: {} | input: {}", e.what(), line);
            std::cout << "{\"event\": \"reject\", \"reason\": \"Internal error\"}\n";
        }
        std::cout.flush();  
    }
    return 0;
}
