//
// Created by Kotarou on 2020/3/19.
//

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "subscriber.h"
#include "../yaml_helper.h"
#include "../utils.h"

Subscriber::Subscriber() {
    this->regex_collapse = false;
    this->enable_grouping = false;
    this->exclude_amplified_node = false;
    this->use_emoji = false;
    this->interval = 300;
}

Subscriber::~Subscriber() = default;

void Subscriber::grouping(size_t group_min_size) {
    auto netflix = node_vector();
    auto leftover = node_vector();

    // cleaner
    std::vector<std::string> remove_list;
    const auto map_cleaner = [&]() {
        for (const auto &name: remove_list) {
            group_result.erase(name);
            spdlog::debug("Remove group {}", name);
        }
        remove_list.clear();
    };

    if (enable_grouping) {
        spdlog::info("Grouping proxies by name, minimum size for a group is {}", group_min_size);
        // grouping
        for (auto proxy : proxies) {
            // trim proxy name
            proxy["name"] = Utils::trim_copy(proxy["name"].as<std::string>());
            auto proxy_name = proxy["name"].as<std::string>();
            auto attribute = parse_name(proxy_name);
            spdlog::trace("proxy name: {}, id: {}, netflix: {}, amplification: {}",
                          attribute.location, attribute.id, attribute.netflix, attribute.amplification);

            // write attributes to proxy
            append_attributes(attribute, proxy);

            if (exclude_amplified_node && attribute.amplification > 1.0) {
                spdlog::debug("Proxy {} excluded, because the amplification is {}", proxy_name,
                              attribute.amplification);
                continue;
            }

            // initialize vector
            if (!group_result.count(attribute.location)) {
                group_result.insert({attribute.location, node_vector()});
            }

            // insert into map
            group_result.at(attribute.location).push_back(proxy);

            // insert into netflix map
            if (attribute.netflix) {
                netflix.push_back(proxy);
            }
        }

        // post processing
        for (auto const &merge_pair: provider["group_merge"]) {
            auto source_name = merge_pair.first.as<std::string>();
            auto target_name = merge_pair.second.as<std::string>();
            if (group_result.count(source_name) && group_result.count(target_name)) {
                spdlog::debug("Move all proxies in group {} to {}", source_name, target_name);
                auto &source_vec = group_result.at(source_name);
                auto &target_vec = group_result.at(target_name);

                target_vec.insert(target_vec.end(), source_vec.begin(), source_vec.end());
                remove_list.emplace_back(source_name);
            }
        }
        map_cleaner();

        // move group size > group_min_size to left over
        for (const auto &node: group_result) {
            if (node.second.size() < group_min_size) {
                leftover.insert(leftover.end(), node.second.begin(), node.second.end());
                remove_list.emplace_back(node.first);
            }
        }
        map_cleaner();
    } else {
        spdlog::info("Grouping proxy is disabled");
        for (auto proxy : proxies) {
            // trim proxy name
            proxy["name"] = Utils::trim_copy(proxy["name"].as<std::string>());
            leftover.emplace_back(proxy);
        }
    }

    if (!netflix.empty()) {
        spdlog::debug("Found {} netflix proxies", netflix.size());
        group_result.insert({"netflix", netflix});
    }

    if (!leftover.empty()) {
        spdlog::debug("Found {} leftover proxies", leftover.size());
        group_result.insert({"leftover", leftover});
    }
}

YAML::Node Subscriber::get() {
    auto node = YAML::Node();

    node["groups"] = YAML::Node(YAML::NodeType::Sequence);
    node["proxies"] = YAML::Node(YAML::NodeType::Sequence);
    node["group_name"] = YAML::Node(YAML::NodeType::Sequence);
    if (!group_result.empty()) {
        auto prefix = provider["prefix"].IsDefined() ? provider["prefix"].as<std::string>() : "Generated";
        for (const auto &group: group_result) {
            auto group_name = fmt::format("{}-{}", prefix, group.first);
            node["group_name"].push_back(group_name);
            spdlog::debug("Processing group {}", group_name);

            auto proxy_group_type = (group.first != "leftover") ? ProxyGroupType::URL_TEST : ProxyGroupType::SELECT;
            auto group_content = YAMLHelper::create_proxy_group(group_name, proxy_group_type, benchmarking_url, interval);
            node["groups"].push_back(group_content);

            size_t counter = 1;
            for (const auto &proxy: group.second) {
                const auto name_generator = [&](const YAML::Node &attributes) {
                    auto id = attributes["id"].as<int>();
                    auto name = attributes["location"].as<std::string>();
                    if (id == -1) {
                        id = counter++;
                    }

                    return use_emoji ? fmt::format("{}{:>02d}", name2emoji(name), id) : proxy["name"].as<std::string>();
                };

                if (proxy.IsDefined() && proxy.IsMap()) {
                    auto proxy_ref = proxy;
                    std::string proxy_name = proxy_ref["name"].as<std::string>();
                    spdlog::trace("Add proxy {} to group {}", proxy_name, group_name);

                    // only update name when grouping is enabled
                    if (proxy_ref["attributes"].IsDefined()) {
                        proxy_name = name_generator(proxy_ref["attributes"]);
                        proxy_ref["name"] = proxy_name;
                    }

                    // do not append duplicated proxy
                    if (group.first != "netflix") {
                        node["proxies"].push_back(proxy_ref);
                    }
                    group_content["proxies"].push_back(proxy_name);
                    // strip attributes
                    proxy_ref.remove("attributes");
                }
            }
        }
    }

    return node;
}

std::string Subscriber::name2emoji(const std::string &name) {
    if (emoji_map[name].IsDefined()) {
        return emoji_map[name].as<std::string>();
    }

    spdlog::info("No emoji defined for {}", name);

    return name;
}

Subscriber::NameAttribute Subscriber::parse_name(const std::string &name) {
    std::smatch match;
    NameAttribute attribute{.location = name, .id = -1, .netflix = false, .amplification = 1.0f};
    if (std::regex_match(name, match, name_parser)) {
        spdlog::trace("Name {}, total number of matches are {}", name, match.size());
        auto regex_result = get_regex_result(match);

        auto get_value = [&](const std::string &key_name, const std::string &default_value) {
            auto def_index = provider["definition"][key_name].as<int>();
            if (def_index != -1 && static_cast<long>(regex_result.size()) > def_index) {
                return regex_result[def_index];
            }

            return default_value;
        };

        // do have an emoji mapper
        if (provider["definition"].IsDefined()) {
            attribute.location = get_value("location_name", "");
            if (!attribute.location.empty()) {
                attribute.id = std::stoi(get_value("position", "-1"));
                attribute.netflix = !get_value("netflix", "").empty();
                auto amplification = get_value("amplification", "1.0f");
                attribute.amplification = std::stof(amplification.empty() ? "1.0f" : amplification);
            }
        }
    }

    return attribute;
}

void Subscriber::append_attributes(const Subscriber::NameAttribute &attribute, YAML::Node &node) {
    node["attributes"] = YAML::Node(YAML::NodeType::Map);
    node["attributes"].force_insert("location", attribute.location);
    node["attributes"].force_insert("id", attribute.id);
    node["attributes"].force_insert("netflix", attribute.netflix);
    node["attributes"].force_insert("amplification", attribute.amplification);
}

std::vector<std::string> Subscriber::get_regex_result(const std::smatch &result) {
    std::vector<std::string> regex_result;
    std::transform(result.begin(), result.end(), std::back_inserter(regex_result), [](const auto &m) {
        return m.str();
    });

    if (regex_collapse) {
        auto it = std::remove_if(regex_result.begin(), regex_result.end(), [](const auto &m) {
            return m.empty();
        });
        regex_result.erase(it, regex_result.end());
    }

    return regex_result;
}

void Subscriber::set_grouping(bool flag) {
    this->enable_grouping = flag;
}

void Subscriber::set_provider(const YAML::Node &_provider) {
    this->provider = _provider;
}

void Subscriber::set_benchmarking_url(const std::string &_benchmarking_url) {
    this->benchmarking_url = _benchmarking_url;
}

void Subscriber::set_use_emoji(bool _use_emoji) {
    this->use_emoji = _use_emoji;
}

void Subscriber::set_benchmarking_interval(unsigned _interval) {
    this->interval = _interval;
}

void Subscriber::set_emoji_map(const YAML::Node &_emoji_map) {
    this->emoji_map = _emoji_map;
}

void Subscriber::set_exclude_amplified_node(bool flag) {
    this->exclude_amplified_node = flag;
}

void Subscriber::set_regex_collapse(bool flag) {
    this->regex_collapse = flag;
}

void Subscriber::set_name_parser(const std::string &pattern) {
    this->name_parser = std::regex(pattern, std::regex_constants::icase | std::regex_constants::ECMAScript);
}
