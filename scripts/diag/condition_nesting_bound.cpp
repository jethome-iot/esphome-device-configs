#include <ArduinoJson.h>
#include <cstdio>
#include <string>
// Replicates the shape ConditionConfig::deserialize walks: an object with "type"
// and a "conditions" array of objects, nested k deep, innermost a leaf.
static std::string build(int k) {
  std::string head, tail;
  for (int i = 0; i < k - 1; i++) {
    head += R"({"type":"and","conditions":[)";
    tail = "]}" + tail;
  }
  head += R"({"type":"input","object_id":"x"})";
  return std::string(R"({"name":"n","trigger":{"type":"startup"},"condition":)") + head + tail +
         R"(,"actions":[{"type":"delay","delay_ms":1}]})";
}
static int walk(JsonObjectConst o, int depth) {
  int deepest = depth;
  if (!o["conditions"].isNull())
    for (JsonVariantConst v : o["conditions"].as<JsonArrayConst>())
      deepest = std::max(deepest, walk(v.as<JsonObjectConst>(), depth + 1));
  return deepest;
}
int main() {
  printf("ARDUINOJSON_DEFAULT_NESTING_LIMIT = %d\n", ARDUINOJSON_DEFAULT_NESTING_LIMIT);
  for (int k = 1; k <= 9; k++) {
    std::string s = build(k);
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, s);
    int d = e ? -1 : walk(doc["condition"].as<JsonObjectConst>(), 1);
    printf("nested ConditionConfig levels=%d -> deserializeJson: %-12s  recursion depth reached=%d\n",
           k, e.c_str(), d);
  }
}
