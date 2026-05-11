#include "ball_stress.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    try {
        const ashiato::sync::stress::StressConfig config = ashiato::sync::stress::parse_args(argc, argv);
        const ashiato::sync::stress::StressReport report = ashiato::sync::stress::run_stress(config);
        if (config.json) {
            ashiato::sync::stress::write_report_json(std::cout, report);
        } else {
            ashiato::sync::stress::write_report_text(std::cout, report);
        }
        return 0;
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()) == "help") {
            ashiato::sync::stress::write_usage(std::cout);
            return 0;
        }
        std::cerr << "error: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
    }

    ashiato::sync::stress::write_usage(std::cerr);
    return 1;
}

