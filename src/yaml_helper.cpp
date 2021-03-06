//
// Created by Kotarou on 2020/3/16.
//
#include <fmt/format.h>
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <version.h>
#include <fstream>

#include "yaml_helper.h"
#include "httpclient.h"
#include "utils.h"
#include "filesystem.h"
#include "exception/file_write_exception.h"
#include "exception/missing_key_exception.h"
#include "exception/file_system_exception.h"
#include "exception/invalid_value_exception.h"

std::string get_group_type_name(ProxyGroupType);

std::string get_provider_type_name(ProviderType);

YAML::Node YAMLHelper::load_remote(const std::string &uri) {
    auto remote_config = HttpClient::get(uri);
    return YAML::Load(remote_config);
}

YAML::Node YAMLHelper::load_local(const std::string &path) {
    if (FileSystem::exists(path)) {
        return YAML::LoadFile(path);
    }

    throw FileSystemException(fmt::format("File {} doesn't exist", path));
}

std::string YAMLHelper::search_key(const YAML::Node &node, const std::vector<std::string> &keys) {
    if (node.IsDefined() && !node.IsScalar()) {
        for (const auto &key :keys) {
            if (node[key].IsDefined()) {
                return key;
            }
        }
    }

    throw MissingKeyException(fmt::format("Unable to find the required key \"{}\"", keys.front()));
}

void YAMLHelper::write_yaml(const YAML::Node &node, const std::string &file) {
    std::ofstream fout(file);

    if (fout.is_open()) {
        spdlog::info("Writing yaml to file {}", file);
        fout << fmt::format("# Generated by {} at {}\n", get_version(), Utils::get_time("%c"));
        fout << node;
        fout.close();
    } else {
        throw FileWriteException(fmt::format("Unable to write file {}", file));
    }
}

void YAMLHelper::node_renamer(const YAML::Node &node, const std::map<std::string, std::string> &replace_pair) {
    for (auto item: node) {
        auto key_name = item.first.as<std::string>();
        if (replace_pair.count(key_name)) {
            item.first = replace_pair.at(key_name);
            spdlog::trace("Replace key {} to {}", key_name, replace_pair.at(key_name));
            continue;
        }
    }
}

void YAMLHelper::node_renamer(const YAML::Node &node, const std::string &search, const std::string &replace) {
    for (auto item: node) {
        auto key_name = item.first.as<std::string>();
        if (key_name == search) {
            item.first = replace;
            spdlog::trace("Replace key {} to {}", key_name, replace);
            break;
        }
    }
}

void YAMLHelper::node_merger(const YAML::Node &source_node, YAML::Node target_node) {
    for (const auto &node : source_node) {
        target_node.push_back(YAML::Node(node));
    }
}

YAML::Node YAMLHelper::create_proxy_group(const std::string &group_name, ProxyGroupType proxyGroupType,
                                          const std::string &url, unsigned interval) {
    auto group_content = YAML::Node();
    group_content["name"] = YAML::Node(group_name);
    group_content["type"] = YAML::Node(get_group_type_name(proxyGroupType));
    group_content["url"] = YAML::Node(url);
    group_content["interval"] = YAML::Node(interval);
    group_content["proxies"] = YAML::Node(YAML::NodeType::Sequence);

    return group_content;
}

YAML::Node YAMLHelper::create_provider_group(ProviderType providerType, const std::string &path, const std::string &url,
                                             bool hc_enable, const std::string &hc_url, unsigned hc_interval) {
    auto group_content = YAML::Node();
    group_content["type"] = YAML::Node(get_provider_type_name(providerType));

    if (providerType == ProviderType::HTTP) {
        if (!url.empty()) {
            group_content["url"] = YAML::Node(url);
        } else {
            throw MissingKeyException("Provider type http must be used with a valid url");
        }
    }

    group_content["path"] = YAML::Node(path);
    group_content["health-check"] = YAML::Node(YAML::NodeType::Map);
    group_content["health-check"]["enable"] = YAML::Node(hc_enable);
    group_content["health-check"]["url"] = YAML::Node(hc_url);
    group_content["health-check"]["interval"] = YAML::Node(hc_interval);

    return group_content;
}

std::string get_group_type_name(ProxyGroupType proxyGroupType) {
    switch (proxyGroupType) {
        case ProxyGroupType::SELECT:
            return "select";
        case ProxyGroupType::RELAY:
            return "relay";
        case ProxyGroupType::URL_TEST:
            return "url-test";
        case ProxyGroupType::FALLBACK:
            return "fallback";
        case ProxyGroupType::LOAD_BALANCE:
            return "load-balance";
    }

    throw InvalidValueException("The value of enumerate ProxyGroupType is invalid");
}

std::string get_provider_type_name(ProviderType providerType) {
    switch (providerType) {
        case ProviderType::FILE:
            return "file";
        case ProviderType::HTTP:
            return "http";
    }

    throw InvalidValueException("The value of enumerate ProviderType is invalid");
}