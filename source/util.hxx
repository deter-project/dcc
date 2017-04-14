#pragma once

#include <vector>
#include <string>

namespace deter
{

///
/// Split
///

std::vector<std::string> & 
split(const std::string &s, char delim, std::vector<std::string> &elems); 

std::vector<std::string> split(const std::string &s, char delim);

std::string erase(const std::string & src, std::string what);

///
/// Cmd
///
struct CmdResult
{
  std::string output;
  int code{0};
};

CmdResult exec(std::string cmd);

//exec with logging
CmdResult execl(std::string cmd);

}
