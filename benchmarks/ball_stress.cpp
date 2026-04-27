#include "ball_stress.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    try {
        const kage::sync::stress::StressConfig config = kage::sync::stress::parse_args(argc, argv);
        const kage::sync::stress::StressReport report = kage::sync::stress::run_stress(config);
        if (config.json) {
            kage::sync::stress::write_report_json(std::cout, report);
        } else {
            kage::sync::stress::write_report_text(std::cout, report);
        }
        return 0;
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()) == "help") {
            kage::sync::stress::write_usage(std::cout);
            return 0;
        }
        std::cerr << "error: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
    }

    kage::sync::stress::write_usage(std::cerr);
    return 1;
}

