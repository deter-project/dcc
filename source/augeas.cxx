#include "augeas.hxx"
#include <stdexcept>

using std::vector;
using std::string;
using std::runtime_error;
using std::experimental::optional;
using std::experimental::make_optional;
using namespace deter;

Augeas::Augeas()
{
  aug_ = aug_init(nullptr, nullptr, 0);
  if(aug_ == nullptr)
    throw runtime_error{"augeas init failure"};
}

Augeas::~Augeas()
{
  aug_close(aug_);
}

vector<string> Augeas::match(string path)
{
  char **matches{nullptr};
  int n = aug_match(aug_, path.c_str(), &matches);
  if(n < 0) return vector<string>{};

  vector<string> result;
  result.reserve(n);
  for(int i=0; i<n; ++i)
  {
    result.push_back(matches[i]);
    free(matches[i]);
  }
  free(matches);
  return result;
}

optional<string> Augeas::get(string path)
{
  const char *label{nullptr};
  int found = aug_get(aug_, path.c_str(), &label);
  if(found) return make_optional(string{label});
  else return optional<string>{};
}

void Augeas::set(string path, string key, string value)
{
  string path_key = path + "/" + key;
  aug_set(aug_, path_key.c_str(), value.c_str());
}

void Augeas::clear(string path, string key)
{
  string path_key = path + "/" + key;
  aug_rm(aug_, path_key.c_str());
}

void Augeas::load()
{
  aug_load(aug_);
}

void Augeas::save()
{
  aug_save(aug_);
}
