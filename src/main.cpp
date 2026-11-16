#include "json.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

using Object = json::Value::Object;
using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t stopping=0;
static void stop(int) { stopping=1; }
struct Error { int status; std::string message; };
struct Socket { int fd; ~Socket() { if(fd>=0) ::close(fd); } };
struct Request { std::string method,path,query,body; std::map<std::string,std::string> headers; };
struct Response { int status=200; json::Value body; std::map<std::string,std::string> headers; };
static std::string lower(std::string s) { for(char& c:s) if(c>='A'&&c<='Z') c=char(c+32); return s; }
static std::string trim(std::string s) { auto a=s.find_first_not_of(" \t"); if(a==std::string::npos) return ""; return s.substr(a,s.find_last_not_of(" \t")-a+1); }
static unsigned number(const std::string& s, unsigned max) { unsigned n=0; auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),n); if(e!=std::errc{}||p!=s.data()+s.size()||s.empty()||n>max) throw Error{400,"Invalid integer"}; return n; }
static bool token(const std::string& s) { return !s.empty()&&std::all_of(s.begin(),s.end(),[](unsigned char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||std::string("!#$%&'*+-.^_`|~").find(char(c))!=std::string::npos; }); }
static Request read_request(int fd) {
    std::string data; char buffer[4096]; size_t end;
    const auto deadline=Clock::now()+std::chrono::seconds(10);
    auto read_more=[&] { if(Clock::now()>deadline) throw Error{408,"Request timed out"}; auto n=recv(fd,buffer,sizeof(buffer),0); if(n<=0) throw Error{408,"Incomplete request or timeout"}; data.append(buffer,size_t(n)); };
    while((end=data.find("\r\n\r\n"))==std::string::npos) { if(data.size()>16384) throw Error{431,"Headers too large"}; read_more(); }
    if(end>16384) throw Error{431,"Headers too large"};
    std::istringstream stream(data.substr(0,end)); std::string line,version,extra; Request r;
    std::getline(stream,line); if(!line.empty()&&line.back()=='\r') line.pop_back(); std::istringstream first(line); first>>r.method>>r.path>>version;
    if(first>>extra || version!="HTTP/1.1" || !token(r.method) || r.path.empty() || r.path[0]!='/') throw Error{400,"Invalid HTTP request line"};
    if(r.path.find_first_of("\r\n\t#")!=std::string::npos) throw Error{400,"Invalid request target"};
    while(std::getline(stream,line)) { if(!line.empty()&&line.back()=='\r') line.pop_back(); auto colon=line.find(':'); if(colon==std::string::npos||!token(line.substr(0,colon))) throw Error{400,"Invalid header"}; auto key=lower(line.substr(0,colon)); auto v=trim(line.substr(colon+1)); for(unsigned char c:v) if((c<32&&c!='\t')||c==127) throw Error{400,"Invalid header value"}; if(!r.headers.emplace(key,v).second) throw Error{400,"Duplicate header"}; }
    if(!r.headers.contains("host")||r.headers.at("host").empty()) throw Error{400,"Host header required"};
    if(r.headers.contains("transfer-encoding")) throw Error{400,"Transfer-Encoding is unsupported; use Content-Length"};
    if(r.headers.contains("expect")) throw Error{417,"Expect is unsupported"};
    unsigned length=r.headers.contains("content-length")?number(r.headers.at("content-length"),4294967295U):0;
    if(length>65536) throw Error{413,"Body exceeds 64 KiB"};
    while(data.size()-end-4<length) read_more();
    r.body=data.substr(end+4,length); auto q=r.path.find('?'); if(q!=std::string::npos) { r.query=r.path.substr(q+1); r.path.resize(q); } return r;
}
static std::string reason(int code) { switch(code) { case 200:return "OK"; case 201:return "Created"; case 204:return "No Content"; case 400:return "Bad Request"; case 401:return "Unauthorized"; case 404:return "Not Found"; case 405:return "Method Not Allowed"; case 408:return "Request Timeout"; case 409:return "Conflict"; case 413:return "Content Too Large"; case 415:return "Unsupported Media Type"; case 417:return "Expectation Failed"; case 422:return "Unprocessable Content"; case 429:return "Too Many Requests"; case 431:return "Request Header Fields Too Large"; case 503:return "Service Unavailable"; default:return "Internal Server Error"; } }
static void send_response(int fd, const Response& r, unsigned long long id) {
    std::string body=r.status==204?"":json::dump(r.body);
    std::string wire="HTTP/1.1 "+std::to_string(r.status)+" "+reason(r.status)+"\r\nContent-Type: application/json; charset=utf-8\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Request-ID: "+std::to_string(id)+"\r\nContent-Length: "+std::to_string(body.size())+"\r\n";
    for(const auto& [k,v]:r.headers) wire+=k+": "+v+"\r\n";
    wire+="\r\n"+body;
    size_t pos=0; while(pos<wire.size()) { auto n=send(fd,wire.data()+pos,wire.size()-pos,MSG_NOSIGNAL); if(n<=0) break; pos+=size_t(n); }
}
struct Task { unsigned id,version=1; std::string title; bool completed=false; json::Value value() const { return Object{{"id",double(id)},{"version",double(version)},{"title",title},{"completed",completed}}; } };
class App {
    std::string api_token;
    std::mutex mutex;
    std::map<unsigned,Task> tasks;
    unsigned next=1;
    std::atomic<unsigned long long> requests{0};
    std::mutex rate_mutex;
    struct Bucket { double tokens=60; Clock::time_point last=Clock::now(); };
    std::map<std::string,Bucket> buckets;
    static Object payload(const Request& r) {
        auto it=r.headers.find("content-type"); if(it==r.headers.end()||lower(trim(it->second.substr(0,it->second.find(';'))))!="application/json") throw Error{415,"Content-Type must be application/json"};
        try { return json::Parser(r.body).parse().object(); } catch(...) { throw Error{400,"Body must be a valid JSON object"}; }
    }
    static void validate(const Object& o, bool create) {
        if(create&&!o.contains("title")) throw Error{422,"title is required"};
        if(o.empty()) throw Error{422,"At least one field is required"};
        for(auto& [key,v]:o) { if(key=="title") { auto p=std::get_if<std::string>(&v.data); if(!p||p->size()>200||trim(*p).empty()) throw Error{422,"title must contain 1 to 200 bytes"}; } else if(key=="completed") { if(!std::holds_alternative<bool>(v.data)) throw Error{422,"completed must be boolean"}; } else throw Error{422,"Unknown field: "+key}; }
    }
public:
    explicit App(std::string t):api_token(std::move(t)) {}
    bool limited(const std::string& ip) {
        std::lock_guard lock(rate_mutex); auto now=Clock::now();
        if(buckets.size()>4096) { for(auto it=buckets.begin();it!=buckets.end();) { if(now-it->second.last>std::chrono::minutes(2)) it=buckets.erase(it); else ++it; } if(buckets.size()>4096&&!buckets.contains(ip)) return true; }
        auto& b=buckets[ip]; b.tokens=std::min(60.0,b.tokens+std::chrono::duration<double>(now-b.last).count()); b.last=now; if(b.tokens<1) return true; b.tokens-=1; return false;
    }
    Response route(const Request& r) {
        ++requests;
        if(r.path=="/health") { if(r.method!="GET") return {405,Object{{"error","Method not allowed"}},{{"Allow","GET"}}}; return {200,Object{{"status","ok"}}, {}}; }
        auto auth=r.headers.find("authorization"); std::string expected="Bearer "+api_token; unsigned diff=0; std::string actual=auth==r.headers.end()?"":auth->second;
        diff=unsigned(actual.size()!=expected.size()); for(size_t i=0;i<expected.size();++i) diff |= unsigned(static_cast<unsigned char>(expected[i]) ^ (i<actual.size()?static_cast<unsigned char>(actual[i]):0));
        if(diff) return {401,Object{{"error","Valid bearer token required"}},{{"WWW-Authenticate","Bearer"}}};
        if(r.path=="/api/v1/stats") { if(r.method!="GET") return {405,Object{{"error","Method not allowed"}},{{"Allow","GET"}}}; std::lock_guard lock(mutex); return {200,Object{{"requests",double(requests.load())},{"tasks",double(tasks.size())}}, {}}; }
        bool collection=r.path=="/api/v1/tasks"; bool item=r.path.starts_with("/api/v1/tasks/"); if(!collection&&!item) throw Error{404,"Route not found"};
        unsigned id=0; if(item) { auto part=r.path.substr(14); id=number(part,1000000000); }
        if(collection&&r.method=="GET") {
            unsigned limit=20,offset=0; std::string filter;
            std::istringstream query(r.query); std::string pair; std::map<std::string,bool> seen;
            while(std::getline(query,pair,'&')) { auto eq=pair.find('='); if(eq==std::string::npos) throw Error{400,"Invalid query"}; auto key=pair.substr(0,eq),v=pair.substr(eq+1); if(!seen.emplace(key,true).second) throw Error{400,"Duplicate query parameter"}; if(key=="limit") { limit=number(v,100); if(limit==0) throw Error{400,"limit must be 1 to 100"}; } else if(key=="offset") offset=number(v,1000000000); else if(key=="completed"&&(v=="true"||v=="false")) filter=v; else throw Error{400,"Unknown or invalid query parameter"}; }
            std::lock_guard lock(mutex); json::Value::Array data; unsigned total=0;
            for(auto& [key,t]:tasks) { (void)key; if(!filter.empty()&&t.completed!=(filter=="true")) continue; if(total++>=offset&&data.size()<limit) data.push_back(t.value()); }
            return {200,Object{{"data",data},{"total",double(total)},{"limit",double(limit)},{"offset",double(offset)}}, {}};
        }
        if(collection&&r.method=="POST") { auto o=payload(r); validate(o,true); std::lock_guard lock(mutex); if(tasks.size()>=10000) throw Error{409,"Task capacity reached"}; Task t{next++,1,std::get<std::string>(o.at("title").data),o.contains("completed")?std::get<bool>(o.at("completed").data):false}; tasks.emplace(t.id,t); return {201,t.value(),{{"Location","/api/v1/tasks/"+std::to_string(t.id)},{"ETag","\"1\""}}}; }
        if(item&&(r.method=="GET"||r.method=="PATCH"||r.method=="DELETE")) {
            Object o; if(r.method=="PATCH") { o=payload(r); validate(o,false); }
            std::lock_guard lock(mutex); auto it=tasks.find(id); if(it==tasks.end()) throw Error{404,"Task not found"}; auto& t=it->second;
            if(r.method!="GET"&&r.headers.contains("if-match")&&r.headers.at("if-match")!="\""+std::to_string(t.version)+"\"") throw Error{409,"Version conflict; fetch the current task and retry"};
            if(r.method=="DELETE") { tasks.erase(it); return {204,nullptr,{}}; }
            if(r.method=="PATCH") { if(o.contains("title")) t.title=std::get<std::string>(o.at("title").data); if(o.contains("completed")) t.completed=std::get<bool>(o.at("completed").data); ++t.version; }
            return {200,t.value(),{{"ETag","\""+std::to_string(t.version)+"\""}}};
        }
        return {405,Object{{"error","Method not allowed"}},{{"Allow",collection?"GET, POST":"GET, PATCH, DELETE"}}};
    }
};
int main() {
    try {
        auto env=[](const char* k,const char* fallback) { const char* v=std::getenv(k); return std::string(v?v:fallback); };
        std::string secret=env("API_TOKEN",""); if(secret.size()<16||!std::all_of(secret.begin(),secret.end(),[](unsigned char c){return c>=33&&c<=126;})) { std::cerr<<"API_TOKEN must contain at least 16 printable ASCII characters without spaces\n"; return 1; }
        unsigned port=number(env("PORT","8080"),65535),workers=number(env("WORKERS","4"),64); if(!port||!workers) throw std::runtime_error("PORT and WORKERS must be positive");
        App app(secret); Socket listener{socket(AF_INET,SOCK_STREAM,0)}; if(listener.fd<0) throw std::runtime_error("socket failed"); int reuse=1; setsockopt(listener.fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(static_cast<uint16_t>(port)); if(inet_pton(AF_INET,env("HOST","127.0.0.1").c_str(),&address.sin_addr)!=1) throw std::runtime_error("HOST must be an IPv4 address");
        if(bind(listener.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0||listen(listener.fd,128)<0) throw std::runtime_error("bind/listen failed");
        std::signal(SIGINT,stop); std::signal(SIGTERM,stop); std::signal(SIGPIPE,SIG_IGN);
        struct Job { int fd; std::string ip; }; std::deque<Job> queue; std::mutex qm,logs; std::condition_variable cv; bool done=false; std::atomic<unsigned long long> sequence{0}; std::vector<std::thread> pool;
        for(unsigned w=0;w<workers;++w) pool.emplace_back([&] { for(;;) { Job job; { std::unique_lock lock(qm); cv.wait(lock,[&]{return done||!queue.empty();}); if(queue.empty()) return; job=queue.front(); queue.pop_front(); }
            Socket client{job.fd}; timeval timeout{2,0}; setsockopt(client.fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)); setsockopt(client.fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
            auto id=++sequence; auto start=Clock::now(); Response result; Request request;
            try { if(app.limited(job.ip)) result={429,Object{{"error","Rate limit exceeded"}},{{"Retry-After","1"}}}; else { request=read_request(client.fd); result=app.route(request); } }
            catch(const Error& e) { result={e.status,Object{{"error",e.message}}, {}}; } catch(...) { result={500,Object{{"error","Internal server error"}}, {}}; }
            send_response(client.fd,result,id); std::lock_guard lock(logs); std::cerr<<json::dump(Object{{"request_id",double(id)},{"method",request.method},{"path",request.path},{"status",result.status},{"duration_ms",double(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count())}})<<'\n';
        } });
        std::cerr<<"Atlas REST listening on "<<env("HOST","127.0.0.1")<<":"<<port<<" with "<<workers<<" workers\n";
        while(!stopping) { fd_set readfds; FD_ZERO(&readfds); FD_SET(listener.fd,&readfds); timeval wait{0,200000}; if(select(listener.fd+1,&readfds,nullptr,nullptr,&wait)<=0) continue; sockaddr_in peer{}; socklen_t size=sizeof(peer); int fd=accept(listener.fd,reinterpret_cast<sockaddr*>(&peer),&size); if(fd<0) continue; char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET,&peer.sin_addr,ip,sizeof(ip)); { std::lock_guard lock(qm); if(queue.size()>=128) { close(fd); continue; } queue.push_back({fd,ip}); } cv.notify_one(); }
        { std::lock_guard lock(qm); done=true; for(auto& job:queue) close(job.fd); queue.clear(); } cv.notify_all(); for(auto& thread:pool) thread.join(); return 0;
    } catch(const Error& e) { std::cerr<<e.message<<'\n'; return 1; } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
