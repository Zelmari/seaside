#include "config.hpp"
#include "session_browser.hpp"

#include <iostream>
#include <nlohmann/json.hpp>

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--browser-defaults")
        return seaside::report_browser_defaults();
    if (argc != 3)
        return 1;
    const seaside::ConfigStore store;
    for (std::string command; std::getline(std::cin, command);) {
        const auto text = store.read(argv[1]);
        if (!text || !*text)
            return 1;
        const auto config = seaside::parse_config(**text);
        if (!config) {
            std::cout << nlohmann::json{{"error", config.error().field}} << std::endl;
            continue;
        }
        const auto status =
            seaside::apply_default_browser(store, argv[2], config->applications.default_browser);
        auto effective = nlohmann::json::object();
        for (std::size_t i = 0; i < seaside::browser_mime_types.size(); ++i) {
            const auto &app = status.effective[i];
            effective[seaside::browser_mime_types[i]] = app ? nlohmann::json(*app) : nullptr;
        }
        std::cout << nlohmann::json{{"requested", status.requested},
                                    {"state", status.state},
                                    {"message", status.message},
                                    {"effective", std::move(effective)}}
                  << std::endl;
    }
}
