#pragma once

#include <cstddef>
#include <string>
#include <typeinfo>

namespace ashiato::sync::detail {

inline bool remove_prefix(std::string& value, const char* prefix) {
    const std::size_t length = std::char_traits<char>::length(prefix);
    if (value.compare(0U, length, prefix) != 0) {
        return false;
    }
    value.erase(0U, length);
    return true;
}

inline std::string short_type_name(std::string name) {
    (void)remove_prefix(name, "struct ");
    (void)remove_prefix(name, "class ");

    const std::string anonymous_namespace = "anonymous namespace";
    const std::size_t anonymous = name.find(anonymous_namespace);
    if (anonymous != std::string::npos) {
        std::size_t begin = anonymous + anonymous_namespace.size();
        while (begin < name.size() && (name[begin] == ')' || name[begin] == '}' ||
               name[begin] == '\'' || name[begin] == '`' || name[begin] == ':' ||
               name[begin] == ' ')) {
            ++begin;
        }
        name.erase(0U, begin);
    }

    const std::size_t scope = name.rfind("::");
    if (scope != std::string::npos) {
        name = name.substr(scope + 2U);
    }
    return name;
}

template <typename T>
std::string default_type_name() {
#if defined(__clang__) || defined(__GNUC__)
    std::string pretty = __PRETTY_FUNCTION__;
    const std::string marker = "T = ";
    const std::size_t begin = pretty.find(marker);
    if (begin != std::string::npos) {
        std::size_t end = pretty.find_first_of(";]", begin + marker.size());
        if (end == std::string::npos) {
            end = pretty.size();
        }
        return short_type_name(pretty.substr(begin + marker.size(), end - (begin + marker.size())));
    }
#endif
    return short_type_name(typeid(T).name());
}

}  // namespace ashiato::sync::detail
