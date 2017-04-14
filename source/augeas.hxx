#pragma once

#include <augeas.h>
#include <vector>
#include <string>
#include <experimental/optional>

namespace deter
{
  class Augeas
  {
    public:
    Augeas();
    ~Augeas();

    // accessors
    std::vector<std::string> match(std::string path);
    std::experimental::optional<std::string> get(std::string path);

    // modifiers
    void set(std::string path, std::string key, std::string value);
    void clear(std::string path, std::string key);
    void load();
    void save();

    
    private:
      augeas *aug_;
  };
}

