#include "util.hxx"
#include <string>
#include <sstream>
#include <vector>
#include <glog/logging.h>
#include <fmt/format.h>

using namespace deter;
using std::vector;
using std::string;
using std::stringstream;

vector<string> & 
deter::split(const string &s, char delim, vector<string> &elems) 
{
  stringstream ss(s);
  string item;
  while (getline(ss, item, delim)) 
  {
    elems.push_back(item);
  }
  return elems;
}


vector<string> deter::split(const string &s, char delim) 
{
  vector<string> elems;
  split(s, delim, elems);
  return elems;
}

string deter::erase(const string & src, string what)
{
  string s_ = src;
  s_.erase(s_.find(what), what.length());
  return s_;
}

CmdResult deter::exec(string cmd)
{
  CmdResult result;
  char buffer[1024];
  //TODO: ghetto redirect, should do something better
  FILE *pipe = popen((cmd + " 2>&1").c_str(), "r");
  if(pipe == nullptr) 
  {
    string msg = fmt::format("exec popen failed for `{}`", cmd);
    LOG(ERROR) << msg;
    result.output = msg;
    result.code = -1;
    return result;
  }
  while(!feof(pipe))
  {
    if(fgets(buffer, 1024, pipe) != nullptr)
      result.output += buffer;
  }

  int pexit = pclose(pipe);
  result.code = WEXITSTATUS(pexit);
  return result;
}

CmdResult deter::execl(string cmd)
{
  CmdResult cr = exec(cmd);
  if(cr.code != 0)
  {
    LOG(WARNING) << "command exec failure";
    LOG(WARNING) << cmd;
    LOG(WARNING) << "exit code: " << cr.code;
    LOG(WARNING) << "output: " << cr.output;
  }
  return cr;
}
