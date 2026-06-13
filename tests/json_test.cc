// Exercises the self-contained JSON parser/serializer: round-trips, escapes,
// numbers, nesting, and malformed-input handling.

#include "json.h"

#include <cassert>
#include <cstdio>

using namespace es::json;

int main() {
    // Parse a representative document.
    Value v = Value::parse(R"({
        "trigger_button": 3,
        "name": "hi\n\"there\"",
        "flag": true,
        "nothing": null,
        "nums": [1, -2.5, 1e3],
        "nested": {"a": [ {"b": 4} ]}
    })");

    assert(v.is_object());
    assert(v["trigger_button"].as_number() == 3);
    assert(v["name"].as_string() == "hi\n\"there\"");
    assert(v["flag"].as_bool() == true);
    assert(v["nothing"].is_null());

    const Array &nums = v["nums"].as_array();
    assert(nums.size() == 3);
    assert(nums[0].as_number() == 1.0);
    assert(nums[1].as_number() == -2.5);
    assert(nums[2].as_number() == 1000.0);

    assert(v["nested"]["a"].as_array()[0]["b"].as_number() == 4);

    // Missing keys yield a null Value rather than throwing.
    assert(v["missing"].is_null());
    assert(v["missing"]["deeper"].is_null());

    // Round-trip: dump then re-parse must preserve values (incl. escapes).
    std::string text = v.dump();
    Value v2 = Value::parse(text);
    assert(v2["name"].as_string() == "hi\n\"there\"");
    assert(v2["nums"].as_array()[1].as_number() == -2.5);
    assert(v2["nested"]["a"].as_array()[0]["b"].as_number() == 4);

    // Integral doubles serialize without a decimal point.
    assert(Value(3.0).dump() == "3");
    assert(Value(-7).dump() == "-7");

    // Empty containers.
    assert(Value(Array{}).dump() == "[]");
    assert(Value(Object{}).dump() == "{}");

    // Malformed inputs throw ParseError.
    int thrown = 0;
    for (const char *bad : {"{", "[1,]", "tru", "\"unterminated", "{\"k\" 1}", "1 2"}) {
        try {
            Value::parse(bad);
        } catch (const ParseError &) {
            ++thrown;
        }
    }
    assert(thrown == 6);

    std::printf("json_test: PASS\n");
    return 0;
}
