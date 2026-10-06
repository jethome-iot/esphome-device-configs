#include "common.h"

namespace esphome::entity_config::testing {

static std::string label_of_json(const char *json) {
  JsonDocument doc = body(json);
  std::string out = "<untouched>";
  if (!parse_label(doc["label"], out))
    return out == "<untouched>" ? "<refused>" : "<refused, but wrote '" + out + "'>";
  return out;
}

// The text rules are panel_text's, and its suite covers them; these are what entity_config adds.
TEST(Label, TheTextRulesArePanelTexts) {
  const std::string text = " Свет ";
  std::string out;
  EXPECT_TRUE(parse_label(text.data(), text.size(), out));
  EXPECT_EQ(out, "Свет");
  EXPECT_EQ(LABEL_MAX_LENGTH, panel_text::LABEL_MAX_LENGTH);
}

TEST(Label, AJsonValueHasToBeAString) {
  EXPECT_EQ(label_of_json(R"({"label":" Kitchen "})"), "Kitchen");
  EXPECT_EQ(label_of_json(R"({"label":""})"), "");
  for (const char *json : {R"({"label":5})", R"({"label":true})", R"({"label":["a"]})", R"({"label":{"a":1}})",
                           R"({"label":null})", R"({})"}) {
    EXPECT_EQ(label_of_json(json), "<refused>") << json;
  }
}

// What JSON escapes decode to goes through the same rules as raw text.
TEST(Label, AnEscapedControlCharacterIsRefused) {
  EXPECT_EQ(label_of_json(R"({"label":"a\u0000b"})"), "<refused>");
  EXPECT_EQ(label_of_json(R"({"label":"a\nb"})"), "<refused>");
  EXPECT_EQ(label_of_json(R"({"label":"\u0007"})"), "<refused>");
  EXPECT_EQ(label_of_json(R"({"label":"Ж"})"), "Ж");
}

TEST(Label, TheMetaFieldIsAStringOfAtMost24) {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  write_label_meta(obj);
  std::string text;
  serializeJson(doc, text);
  EXPECT_EQ(text, R"({"label":{"type":"string","label":"Label","description":"Shown on the panel and the dashboard )"
                  R"(in place of the name. Empty shows the name.","default":"","max_length":24}})");
}

}  // namespace esphome::entity_config::testing
