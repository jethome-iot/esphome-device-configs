#include <ArduinoJson.h>
#include <string>
#include <cstdio>
static std::string build(int levels){
  // innermost: temperature range with min>max
  std::string inner = R"({"type":"temperature","object_id":"t","temperature_type":"range","min_threshold":9,"max_threshold":1})";
  std::string cond = inner;
  for (int i=0;i<levels-1;i++) cond = std::string(R"({"type":"and","conditions":[)") + cond + "]}";
  return std::string(R"({"name":"a","triggers":[{"source":"startup"}],"condition":)") + cond + "}";
}
// count how deep ConditionConfig::deserialize would recurse, by walking the parsed doc
static int walk(JsonObjectConst o){
  if (o["conditions"].isNull()) return 1;
  int m=0; for (JsonVariantConst v : o["conditions"].as<JsonArrayConst>()) { int d=walk(v.as<JsonObjectConst>()); if(d>m)m=d; }
  return 1+m;
}
int main(){
  for(int L=1;L<=9;L++){
    std::string s=build(L);
    JsonDocument doc;
    DeserializationError e=deserializeJson(doc,s);
    printf("levels=%d bytes=%zu err=%-12s recursion_depth=%d\n",L,s.size(),e.c_str(), e?0:walk(doc.as<JsonObjectConst>()["condition"].as<JsonObjectConst>()));
    if(L==5||L==6){ FILE*f=fopen((std::string("payload-")+std::to_string(L)+".json").c_str(),"w"); fwrite(s.data(),1,s.size(),f); fclose(f);}
  }
}
