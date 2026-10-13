#include "os/rpc.hpp"
#include "os/cli.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
int main(int argc, char** argv) {
    try {
        os::Arguments args(argc, argv);
        os::rpc::LoadOptions options;
        options.port = static_cast<std::uint16_t>(args.number("--port", 9000, 1, 65535));
        options.connections = args.number("--connections", 64, 1, 4096);
        options.requests_per_connection = args.number("--requests", 200, 1, 1000000);
        options.warmup_per_connection = args.number("--warmup", 20, 0, 1000000);
        options.pipeline = args.number("--pipeline", 4, 1, 256);
        options.payload_size = args.number("--payload", 64, 0, os::rpc::max_payload - 1);
        auto sample_file = args.take("--samples"); args.finish();
        auto result = os::rpc::load(options);
        if (!sample_file.empty()) {
            std::ofstream output(sample_file); output.exceptions(std::ios::badbit | std::ios::failbit);
            output << "connection,request_id,latency_us\n" << std::fixed << std::setprecision(6);
            for (const auto& s : result.samples) output << s.connection << ',' << s.id << ',' << s.latency_us << '\n';
        }
        std::cout << std::setprecision(12) << "{\"connections\":" << options.connections
            << ",\"requests_per_connection\":" << options.requests_per_connection
            << ",\"warmup_per_connection\":" << options.warmup_per_connection
            << ",\"pipeline\":" << options.pipeline << ",\"payload_bytes\":" << options.payload_size
            << ",\"completed\":" << result.samples.size() << ",\"errors\":0,\"elapsed_seconds\":" << result.elapsed_seconds
            << ",\"throughput_rps\":" << result.samples.size() / result.elapsed_seconds
            << ",\"p50_us\":" << os::rpc::percentile(result.samples, .50)
            << ",\"p95_us\":" << os::rpc::percentile(result.samples, .95)
            << ",\"p99_us\":" << os::rpc::percentile(result.samples, .99) << "}\n";
    } catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
