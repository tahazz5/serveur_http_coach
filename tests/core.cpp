#define main atlas_server_main
#include "../src/main.cpp"
#undef main
#include <functional>

static unsigned checks=0;
static void check(bool condition) { ++checks; if(!condition) throw std::runtime_error("Check failed: "+std::to_string(checks)); }
static void error(int status, const std::function<void()>& fn) { try { fn(); } catch(const Error& e) { check(e.status==status); return; } throw std::runtime_error("Expected error"); }
int main() {
    try {
        for(const std::string text : {"{}","[]","true","null","-12.3e+2","{\"text\":\"\\uD83D\\uDE80\"}"}) check(!json::dump(json::Parser(text).parse()).empty());
        for(const std::string text : {"", "{\"a\":1,\"a\":2}","01","[1,]","1e", "\"\\ud800\"", "false extra"}) { bool rejected=false; try { json::Parser(text).parse(); } catch(...) { rejected=true; } check(rejected); }
        for(const std::string text : {std::string("\"\xc0\xaf\""),std::string("\"\xed\xa0\x80\"")}) {
            bool rejected=false; try { json::Parser(text).parse(); } catch(...) { rejected=true; } check(rejected);
        }
        App app("a-very-long-test-token");
        auto req=[](std::string method,std::string path,std::string body="") { return Request{std::move(method),std::move(path),"",std::move(body),{{"authorization","Bearer a-very-long-test-token"},{"content-type","application/json"}}}; };
        check(app.route(req("GET","/health")).status==200);
        auto unauth=req("GET","/api/v1/tasks"); unauth.headers.clear(); check(app.route(unauth).status==401);
        auto created=app.route(req("POST","/api/v1/tasks","{\"title\":\"task\"}")); check(created.status==201); check(created.headers.at("Location")=="/api/v1/tasks/1");
        check(app.route(req("GET","/api/v1/tasks/1")).headers.at("ETag")=="\"1\"");
        auto patch=req("PATCH","/api/v1/tasks/1","{\"completed\":true}"); patch.headers["if-match"]="\"1\""; auto updated=app.route(patch); check(updated.status==200); check(updated.headers.at("ETag")=="\"2\""); error(409,[&]{ app.route(patch); });
        auto list=req("GET","/api/v1/tasks"); list.query="completed=true&limit=1"; check(json::dump(app.route(list).body).find("\"total\":1")!=std::string::npos);
        list.query="limit=0"; error(400,[&]{app.route(list);});
        error(422,[&]{app.route(req("POST","/api/v1/tasks","{\"title\":false}"));});
        error(400,[&]{app.route(req("POST","/api/v1/tasks","[1]"));});
        check(app.route(req("PUT","/api/v1/tasks")).status==405);
        check(app.route(req("DELETE","/api/v1/tasks/1")).status==204);
        error(404,[&]{app.route(req("GET","/api/v1/tasks/1"));});
        for(int i=0;i<60;++i) check(!app.limited("test")); check(app.limited("test"));
        std::vector<std::thread> writers; std::atomic<int> successes{0};
        for(int i=0;i<8;++i) writers.emplace_back([&]{ for(int j=0;j<100;++j) if(app.route(req("POST","/api/v1/tasks","{\"title\":\"parallel\"}")).status==201) ++successes; });
        for(auto& writer:writers) writer.join(); check(successes==800);
        list.query="limit=100&offset=700"; auto page=app.route(list); check(std::get<double>(page.body.object().at("total").data)==800); check(std::get<json::Value::Array>(page.body.object().at("data").data).size()==100);
        std::cout<<checks<<" checks passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } catch(const Error& e) { std::cerr<<e.message<<'\n'; return 1; }
}
