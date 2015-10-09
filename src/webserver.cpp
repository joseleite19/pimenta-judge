#include <cstring>
#include <string>
#include <map>
#include <set>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "webserver.h"

#include "global.h"
#include "judge.h"
#include "scoreboard.h"

using namespace std;

struct Request {
  string line;
  map<string, string> headers;
  Request(int sd) {
    // read socket
    vector<char> buf; {
      char c;
      for (bool done = false; !done;) {
        while (!done && read(sd, &c, 1) == 1) {
          buf.push_back(c);
          if (c == '\n' && buf.size() >= 4 && buf[buf.size()-4] == '\r') {
            done = true;
          }
        }
      }
    }
    int i = 0;
    
    // parse request line
    for (; buf[i] != '\r'; i++) line += buf[i];
    i += 2;
    
    // parse headers
    while (true) {
      string line;
      for (; buf[i] != '\r'; i++) line += buf[i];
      i += 2;
      if (line == "") break;
      int j = 0;
      string header;
      for (; j < int(line.size()) && line[j] != ':'; j++) header += line[j];
      string content;
      for (j += 2; j < int(line.size()); j++) content += line[j];
      headers[header] = content;
    }
  }
};

struct Client {
  sockaddr_in addr;
  int sd;
};

static pthread_mutex_t login_mutex = PTHREAD_MUTEX_INITIALIZER;
static map<in_addr_t, pair<string, string>> teambyip;
static map<string, set<in_addr_t>> teambylogin;

static void showlogin(int sd) {
  string form =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\r"
    "Content-Type: text/html\r\n"
    "\r\n"
    "<html id=\"root\">\n"
    "  <head>\n"
    "    <script>\n"
    "      function sendform() {\n"
    "        team = document.getElementById(\"team\").value;\n"
    "        pass = document.getElementById(\"pass\").value;\n"
    "        if (window.XMLHttpRequest)\n"
    "          xmlhttp = new XMLHttpRequest();\n"
    "        else\n"
    "          xmlhttp = new ActiveXObject(\"Microsoft.XMLHTTP\");\n"
    "        xmlhttp.onreadystatechange = function() {\n"
    "          if (xmlhttp.readyState == 4 && xmlhttp.status == 200) {\n"
    "            if (xmlhttp.responseText[0] == 'I') {\n"
    "              response = document.getElementById(\"response\");\n"
    "              response.innerHTML = xmlhttp.responseText;\n"
    "            } else window.location = \"/\";\n"
    "          }\n"
    "        }\n"
    "        xmlhttp.open(\"POST\", \"login\", true);\n"
    "        xmlhttp.setRequestHeader(\"Team\", team);\n"
    "        xmlhttp.setRequestHeader(\"Password\", pass);\n"
    "        xmlhttp.send();\n"
    "      }\n"
    "    </script>\n"
    "  </head>\n"
    "  <body>\n"
    "    <h1 align=\"center\">Pimenta Judgezzz~*~*</h1>\n"
    "    <h2 align=\"center\">Login</h2>\n"
    "    <h4 align=\"center\" id=\"response\"></h4>\n"
    "    <table align=\"center\">\n"
    "      <tr>\n"
    "        <td>Team:</td>\n"
    "        <td><input type=\"text\" id=\"team\" autofocus/></td>\n"
    "      </tr>\n"
    "      <tr>\n"
    "        <td>Password:</td>\n"
    "        <td><input type=\"password\" id=\"pass\" /></td>\n"
    "      </tr>\n"
    "      <tr><td><button onclick=\"sendform()\">Login</button></td></tr>\n"
    "    </table>\n"
    "  </body>\n"
    "</html>\n"
  ;
  write(sd, form.c_str(), form.size());
}

static string login(const string& team, const string& password) {
  FILE* fp = fopen("teams.txt", "r");
  if (!fp) return "";
  string name;
  for (fgetc(fp); name == "" && !feof(fp); fgetc(fp)) {
    string ntmp;
    for (char c = fgetc(fp); c != '"'; c = fgetc(fp)) ntmp += c;
    fgetc(fp);
    string tmp;
    for (char c = fgetc(fp); c != ' '; c = fgetc(fp)) tmp += c;
    string pass;
    for (char c = fgetc(fp); c != '\n'; c = fgetc(fp)) pass += c;
    if (team == tmp && password == pass) name = ntmp;
  }
  fclose(fp);
  return name;
}

static void* zenity(void* ptr) {
  string* sptr = (string*)ptr;
  system("zenity --warning --text=\"%s\"", sptr->c_str());
  delete sptr;
  return nullptr;
}

static void statement(int sd) {
  Settings settings;
  string response;
  bool send = false;
  if (time(nullptr) < settings.begin) {
    response =
      "HTTP/1.1 403 Forbidden\r\n"
      "Connection: close\r\r"
      "\r\n"
    ;
  }
  else {
    response =
      "HTTP/1.1 200 OK\r\n"
      "Connection: close\r\r"
      "Content-Type: application/octet-stream\r\n"
      "Content-Disposition: attachment; filename=\"contest.pdf\"\r\n"
      "\r\n"
    ;
    send = true;
  }
  write(sd, response.c_str(), response.size());
  if (!send) return;
  FILE* fp = fopen("contest.pdf", "rb");
  if (!fp) return;
  char* buf = new char[1<<20];
  for (size_t rd = 0; (rd = fread(buf, 1, 1 << 20, fp)) > 0;) {
    write(sd, buf, rd);
  }
  delete[] buf;
  fclose(fp);
}

static void* client(void* ptr) {
  Client* cptr = (Client*)ptr;
  Request req(cptr->sd);
  
  // login
  pthread_mutex_lock(&login_mutex);
  if (teambyip.find(cptr->addr.sin_addr.s_addr) == teambyip.end()) {
    if (req.line[0] == 'P' && req.line.find("login") != string::npos) {
      string teamname = login(req.headers["Team"], req.headers["Password"]);
      if (teamname == "") write(cptr->sd, "Invalid team/password!", 22);
      else {
        auto& ips = teambylogin[req.headers["Team"]];
        ips.insert(cptr->addr.sin_addr.s_addr);
        if (ips.size() > 1) {
          string msg =
            "team "+req.headers["Team"]+" logging with multiple ips:\n"
          ;
          for (in_addr_t x : ips) msg += (to<string>(x)+"\n");
          pthread_t thread;
          pthread_create(&thread, nullptr, zenity, new string(msg));
          pthread_detach(thread);
        }
        teambyip[cptr->addr.sin_addr.s_addr].first = teamname;
        teambyip[cptr->addr.sin_addr.s_addr].second = req.headers["Team"];
        write(cptr->sd, "k", 1);
      }
    }
    else showlogin(cptr->sd);
    pthread_mutex_unlock(&login_mutex);
  }
  // logout
  else if (req.line.find("logout") != string::npos) {
    teambyip.erase(cptr->addr.sin_addr.s_addr);
    pthread_mutex_unlock(&login_mutex);
    write(cptr->sd, "Logged out.", 11);
  }
  else {
    auto& team = teambyip[cptr->addr.sin_addr.s_addr];
    pthread_mutex_unlock(&login_mutex);
    
    // fetch uri
    stringstream ss;
    ss << req.line;
    string uri;
    ss >> uri;
    ss >> uri;
    
    // post
    if (req.line[0] == 'P') {
      if (uri.find("submission") != string::npos) {
        Judge::attempt(
          cptr->sd,
          team.first, team.second,
          req.headers["File-name"], to<int>(req.headers["File-size"])
        );
      }
      else {
        //TODO clarifs.
      }
    }
    // data request
    else if (uri.find("?") != string::npos) {
      if (uri.find("teamname") != string::npos) {
        write(cptr->sd, team.first.c_str(), team.first.size());
      }
      else if (uri.find("scoreboard") != string::npos) {
        Scoreboard::send(cptr->sd);
      }
      else if (uri.find("statement") != string::npos) {
        statement(cptr->sd);
      }
    }
    // file request
    else {
      string ctype;
      if (uri.find(".css") != string::npos) {
        ctype = "text/css";
      }
      else if (uri.find(".js") != string::npos) {
        ctype = "application/javascript";
      }
      else if (uri.find(".gif") != string::npos) {
        ctype = "application/octet-stream";
      }
      else {
        ctype = "text/html";
        uri = "/index.html";
      }
      if (ctype != "") {
        string header =
          "HTTP/1.1 200 OK\r\n"
          "Connection: close\r\r"
          "Content-Type: "+ctype+"\r\n"
          "\r\n"
        ;
        FILE* fp = fopen(("www"+uri).c_str(), "rb");
        if (fp) {
          char* buf = new char[1<<10];
          int sz;
          write(cptr->sd, header.c_str(), header.size());
          while ((sz = fread(buf, 1, 1<<10, fp)) > 0) write(cptr->sd, buf, sz);
          delete[] buf;
          fclose(fp);
        }
      }
    }
  }
  
  close(cptr->sd);
  delete cptr;
  return nullptr;
}

static void* server(void*) {
  // create socket
  int sd = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  fcntl(sd, F_SETFL, FNDELAY);
  
  // set addr
  sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(to<uint16_t>(Global::arg[2]));
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(sd, (sockaddr*)&addr, sizeof addr);
  
  // listen
  listen(sd, SOMAXCONN);
  while (Global::alive()) {
    socklen_t alen;
    Client c;
    c.sd = accept(sd, (sockaddr*)&c.addr, &alen);
    if (c.sd < 0) { usleep(25000); continue; }
    pthread_t thread;
    pthread_create(&thread, nullptr, client, new Client(c));
    pthread_detach(thread);
  }
  
  // close
  close(sd);
  return nullptr;
}

namespace WebServer {

void fire() {
  Global::fire(server);
}

} // namespace WebServer