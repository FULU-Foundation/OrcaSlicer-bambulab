#ifndef __JSON_DIFF_HPP
#define __JSON_DIFF_HPP

#include <string>
#include <atomic>
#include <vector>

#include "nlohmann/json.hpp"
#include <wx/string.h>

namespace nlohmann {
template <>
struct adl_serializer<wxString> {
    static void to_json(json& j, const wxString& value)
    {
        const wxCharBuffer utf8 = value.ToUTF8();
        j = utf8 ? std::string(utf8.data()) : std::string();
    }

    static void from_json(const json& j, wxString& value)
    {
        if (!j.is_string()) {
            value.clear();
            return;
        }
        const auto& s = j.get_ref<const std::string&>();
        value = wxString::FromUTF8(s.c_str());
    }
};
}

using json = nlohmann::json;
using namespace std;

class json_diff
{
private:
    std::string printer_type;
    std::string printer_version = "00.00.00.00";
    json settings_base;
    json full_message;

    json diff2all_base;
    json all2diff_base;
    int  decode_error_count = 0;

    int  diff_objects(json const &in, json &out, json const &base);
    int  restore_objects(json const &in, json &out, json const &base);
    int  restore_append_objects(json const &in, json &out);
    void merge_objects(json const &in, json &out);

public:
    bool load_compatible_settings(std::string const &type, std::string const &version);
    int all2diff(json const &in, json &out);
    int  diff2all(json const &in, json &out);
    int  all2diff_base_reset(json const &base);
    int  diff2all_base_reset(json &base);
    void compare_print(json &a, json &b);

    bool is_need_request();
};
#endif // __JSON_DIFF_HPP
