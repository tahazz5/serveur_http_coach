#pragma once
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace json {
struct Value {
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;
    std::variant<std::nullptr_t, bool, double, std::string, Object, Array> data = nullptr;
    Value() = default;
    Value(std::nullptr_t): data(nullptr) {}
    Value(bool v): data(v) {}
    Value(double v): data(v) {}
    Value(int v): data(double(v)) {}
    Value(std::string v): data(std::move(v)) {}
    Value(const char* v): data(std::string(v)) {}
    Value(Object v): data(std::move(v)) {}
    Value(Array v): data(std::move(v)) {}
    const Object& object() const { return std::get<Object>(data); }
};
inline std::string quote(const std::string& s) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}
inline std::string dump(const Value& v) {
    if (std::holds_alternative<std::nullptr_t>(v.data)) return "null";
    if (auto p = std::get_if<bool>(&v.data)) return *p ? "true" : "false";
    if (auto p = std::get_if<double>(&v.data)) { auto s = std::to_string(*p); while(s.back() == '0') s.pop_back(); if(s.back() == '.') s.pop_back(); return s; }
    if (auto p = std::get_if<std::string>(&v.data)) return quote(*p);
    std::string out;
    if (auto p = std::get_if<Value::Object>(&v.data)) { out = "{"; for (const auto& [k,x] : *p) { if(out.size()>1) out += ','; out += quote(k) + ':' + dump(x); } return out + '}'; }
    out = "["; for(const auto& x : std::get<Value::Array>(v.data)) { if(out.size()>1) out += ','; out += dump(x); } return out + ']';
}
class Parser {
    const std::string& s; size_t i = 0;
    [[noreturn]] void fail() const { throw std::runtime_error("Invalid JSON"); }
    void ws() { while(i<s.size() && (s[i]==' ' || s[i]=='\n' || s[i]=='\r' || s[i]=='\t')) ++i; }
    bool take(char c) { ws(); if(i<s.size() && s[i]==c) { ++i; return true; } return false; }
    unsigned hex4() { unsigned n=0; for(int j=0;j<4;++j) { if(i>=s.size()) fail(); char c=s[i++]; n <<= 4; if(c>='0'&&c<='9') n+=unsigned(c-'0'); else if(c>='a'&&c<='f') n+=unsigned(c-'a'+10); else if(c>='A'&&c<='F') n+=unsigned(c-'A'+10); else fail(); } return n; }
    void utf8(std::string& out, unsigned n) { if(n<128) out+=char(n); else if(n<2048) { out+=char(192|(n>>6)); out+=char(128|(n&63)); } else if(n<65536) { out+=char(224|(n>>12)); out+=char(128|((n>>6)&63)); out+=char(128|(n&63)); } else { out+=char(240|(n>>18)); out+=char(128|((n>>12)&63)); out+=char(128|((n>>6)&63)); out+=char(128|(n&63)); } }
    std::string str() {
        if(!take('"')) fail();
        std::string out;
        while(i<s.size()) { unsigned char c = static_cast<unsigned char>(s[i++]); if(c=='"') return out; if(c<32) fail(); if(c!='\\') { out+=char(c); continue; }
            if(i>=s.size()) fail();
            char e=s[i++];
            switch(e) { case '"': case '\\': case '/': out+=e; break; case 'b': out+='\b'; break; case 'f': out+='\f'; break; case 'n': out+='\n'; break; case 'r': out+='\r'; break; case 't': out+='\t'; break;
            case 'u': { unsigned n=hex4(); if(n>=0xd800&&n<=0xdbff) { if(i+2>s.size()||s[i++]!='\\'||s[i++]!='u') fail(); unsigned low=hex4(); if(low<0xdc00||low>0xdfff) fail(); n=0x10000+((n-0xd800)<<10)+(low-0xdc00); } else if(n>=0xdc00&&n<=0xdfff) fail(); utf8(out,n); break; } default: fail(); }
        } fail();
    }
    Value value(int depth) {
        if(depth>32) fail();
        ws();
        if(i>=s.size()) fail();
        if(s[i]=='"') return str();
        if(take('{')) { Value::Object o; if(take('}')) return o; do { auto key=str(); if(!take(':')) fail(); if(!o.emplace(key,value(depth+1)).second) fail(); } while(take(',')); if(!take('}')) fail(); return o; }
        if(take('[')) { Value::Array a; if(take(']')) return a; do { a.push_back(value(depth+1)); } while(take(',')); if(!take(']')) fail(); return a; }
        for(auto [word,v] : {std::pair{"true",Value(true)}, {"false",Value(false)}, {"null",Value(nullptr)}}) { std::string w=word; if(s.compare(i,w.size(),w)==0) { i+=w.size(); return v; } }
        size_t start=i; if(s[i]=='-') ++i;
        if(i>=s.size()) fail();
        if(s[i]=='0') ++i; else { if(s[i]<'1'||s[i]>'9') fail(); while(i<s.size()&&s[i]>='0'&&s[i]<='9') ++i; }
        if(i<s.size()&&s[i]=='.') { ++i; size_t b=i; while(i<s.size()&&s[i]>='0'&&s[i]<='9') ++i; if(b==i) fail(); }
        if(i<s.size()&&(s[i]=='e'||s[i]=='E')) { ++i; if(i<s.size()&&(s[i]=='+'||s[i]=='-')) ++i; size_t b=i; while(i<s.size()&&s[i]>='0'&&s[i]<='9') ++i; if(b==i) fail(); }
        double n=std::stod(s.substr(start,i-start)); if(!std::isfinite(n)) fail(); return n;
    }
public:
    explicit Parser(const std::string& input): s(input) {}
    Value parse() {
        // Reject overlong encodings, surrogate code points, and truncated UTF-8.
        for(size_t p=0;p<s.size();) {
            unsigned char lead=static_cast<unsigned char>(s[p++]);
            if(lead<128) continue;
            unsigned n=0, code=0, minimum=0;
            if(lead>=0xc2&&lead<=0xdf) { n=1; code=lead&31; minimum=128; }
            else if(lead>=0xe0&&lead<=0xef) { n=2; code=lead&15; minimum=2048; }
            else if(lead>=0xf0&&lead<=0xf4) { n=3; code=lead&7; minimum=65536; }
            else fail();
            if(p+n>s.size()) fail();
            while(n--) { unsigned char c=static_cast<unsigned char>(s[p++]); if((c&192)!=128) fail(); code=(code<<6)|(c&63); }
            if(code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff)) fail();
        }
        Value v=value(0); ws(); if(i!=s.size()) fail(); return v; }
};
}
