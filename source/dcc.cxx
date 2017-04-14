#include <iostream>
#include <regex>
#include <thread>
#include <stdexcept>
#include <sstream>
#include <chrono>
#include <fstream>
#include "dcc.hxx"
#include "util.hxx"
#include <fmt/format.h>
#include "pipes.hxx"
#include "netlink.hxx"
#include <glog/logging.h>

using std::vector;
using std::string;
using std::regex;
using std::smatch;
using std::regex_match;
using std::experimental::optional;
using std::experimental::make_optional;
using std::pair;
using std::make_pair;
using std::ostream;
using std::ofstream;
using std::ifstream;
using std::stringstream;
using std::thread;
using std::to_string;
using std::stoul;
using std::runtime_error;
namespace chrono = std::chrono;
using std::milli;
using namespace deter;

/*TODO: we are reloading the augeas cache on every call, this is fairly
 * expensive based on observation. At some point we should use a file
 * watcher to only reload when needed
 */

///
/// static helpers
///
static bool ifup(string ifx)
{
  auto cr = execl(fmt::format("ifup {}", ifx));
  return (cr.code == 0);
}

static bool cycleInterface(string ifx)
{
  bool result = true;

  auto cr = execl(fmt::format("ifdown {}", ifx));
  result &= (cr.code == 0);

  cr = execl(fmt::format("ifup {}", ifx));
  if(cr.code != 0) return false;
  result &= (cr.code == 0);

  return result;
}


/* -----------------------------------------------------------------------------
 *  ~ VlanInfo
 */

VlanInfo::VlanInfo(size_t id) : deterId{id}, cumulusId{id} {}

VlanInfo::VlanInfo(size_t id, vector<string> members) 
  : deterId{id}, cumulusId{id}, members{members}
{}

VlanInfo::VlanInfo(size_t did, size_t cid, vector<string> members) 
  : deterId{did}, cumulusId{cid}, members{members}
{}



/* -----------------------------------------------------------------------------
 *  ~ Dcc
 */

/*
 * Dcc -- Public API
 */

vector<VlanInfo> Dcc::listVlans()
{
  LOG(INFO) << "listVlans()";
  aug_.load();

  //if there is no bridge there are no vlans
  auto bpo = bridgePath();
  if(!bpo) return vector<VlanInfo>{};
  auto bp = *bpo;

  //get the vlan identifiers from the bridge config
  string vs = aug_.get(bp + bridge_vids).value_or("");
  if(vs.empty()) 
  {
    return vector<VlanInfo>{};
  }

  using namespace pipes;

  return
  split(vs, ' ') 
    | map([&](const string & v)
      { 
        size_t id = stoul(v);
        return VlanInfo{id, vlanMembers(id)};
      });
}

void Dcc::disablePortTrunking(string ifx, bool finalize)
{
  LOG(INFO) << "diablePortTrunking(" << ifx << ")";
  aug_.load();
  auto result = aug_.match(fmt::format("{}/iface[ . = '{}' ]", ifx_path, ifx));
  if(result.empty()) 
  {
    LOG(ERROR) << "could not find interface " << ifx;
  }

  aug_.set(result[0], "bridge-allow-untagged", "yes");
  aug_.clear(result[0], "brige-vids");

  if(finalize)
  {
    aug_.save();
    ifup(ifx);
  }
}


bool Dcc::enablePortTrunking(string ifx, size_t vlan_id, bool /*eq_trunk*/)
{
  LOG(INFO) << "enablePortTrunking("
    << ifx << ","
    << vlan_id << ")";

  aug_.load();
  auto result = aug_.match(fmt::format("{}/iface[ . = '{}' ]", ifx_path, ifx));
  if(result.empty()) 
  {
    LOG(ERROR) << "could not find interface " << ifx;
    return false;
  }

  aug_.set(result[0], "bridge-allow-untagged", "no");

  //migrate access vids to trunk vids
  string path={result[0]+"/bridge-access"};
  auto _existing = aug_.get(path);
  string existing{""};
  if(_existing) {
    existing = *_existing + " ";
  }
  existing += to_string(vlan_id);
  aug_.set(result[0], "bridge-vids", existing);


  aug_.save();

  ifup(ifx);
  
  return true;
}


void Dcc::setIfxVids(string ifx, vector<size_t> vlans, bool allow)
{
  auto path = ifxPath(ifx);
  auto existingVlans = aug_.get(path+"/bridge-vids");
  vector<size_t> vs;
  if(existingVlans) { vs = parseVlist(*existingVlans); }

  if(allow) 
  {
    for(auto v : vlans) addVlan(v, vs);
  }
  else
  {
    for(auto v : vlans) { removeVlan(v, vs); }
  }

  auto value = emitVlist(vs);

  LOG(INFO) << ifx << " vlans: " << value;

  aug_.set(path, "bridge-vids", value);

}

void Dcc::setVlansOnTrunk(string ifx, vector<size_t> vlans, bool allow)
{
  LOG(INFO) << "setVlansOnTrunk("
    << ifx << ","
    << "[...],"
    << allow << ")";

  aug_.load();

  setIfxVids(ifx, vlans, allow);
  setIfxVids("bridge", vlans, allow);

  aug_.save();
  
  ifup(ifx);
  ifup("bridge");
}


vector<pair<size_t, optional<size_t>>> Dcc::findVlans(vector<size_t> ids)
{
  LOG(INFO) << "findVlans([...])";

  using namespace pipes;
  auto vlanInfo = listVlans();

  if(ids.empty())
  {
    return vlanInfo 
      | map([](const VlanInfo &v) 
        { 
          return make_pair(v.cumulusId, make_optional(v.deterId)); 
        });
  }

  return
  ids 
    | map([&vlanInfo](size_t id)
      {
        auto result = make_pair(id, optional<size_t>{});
        auto i = 
        find_if(vlanInfo.begin(), vlanInfo.end(), 
          [id](const VlanInfo & v){ return v.cumulusId == id; });

        if(i != vlanInfo.end()) result.second = i->deterId;

        return result;
      });
}
      
bool Dcc::vlanHasPorts(size_t vlan_id)
{
  LOG(INFO) << "vlanHasPorts(" << vlan_id << ")";

  aug_.load();
  auto vs = listVlans();
  for(const auto & v : vs)
  {
    if(v.cumulusId == vlan_id && !v.members.empty()) return true;
  }
  return false;
}

void Dcc::updateActiveInterfaces()
{
  using namespace pipes;

  aug_.match("/files/etc/network/interfaces/iface")
    | for_each([this](const string &x){ activeIfxs_.insert(*aug_.get(x)); });
}

vector<Interface> Dcc::getInterfaces()
{
  LOG(INFO) << "getInterfaces()";

  updateActiveInterfaces();

  vector<Interface> ixs;

  auto response = NetLink::getLink();
  for(const auto & m : response.messages)
  {
    Interface ix;
    auto name = m.getAttribute<string>(IFLA_IFNAME);
    ix.name = name;
    
    //only care about physical interfaces
    //if(name.compare(0, 3, "swp") != 0)
    if(name == "bridge" || activeIfxs_.find(name) == activeIfxs_.end())
    {
      continue;
    }

    //TODO it seems we may not even need this, linkSpeed
    //will report the speed even when the link is down if
    //the module is indeed plugged in and it also does
    //not require root for the SIOCETHTOOL ioctl which is nice
    //ix.capSpeed = NetLink::capSpeed(name);
    //
    //also reading the modlue information off the eeprom
    //is slow as a turd, so not having to do that is nice
    
    //yeah its gross, fix later
    if(name.compare(0, 3, "swp") == 0)
    {
      ix.linkSpeed = NetLink::linkSpeed(name);
    }
    else if(name.compare(0, 4, "leaf") == 0)
    {
      ix.linkSpeed = 40000*4;
    }
    else if(name.compare(0, 5, "spine") == 0)
    {
      ix.linkSpeed = 40000*4;
    }
    else if(name.compare(0, 6, "uplink") == 0)
    {
      ix.linkSpeed = 40000*2;
    }

    ix.enabled = ((m.ifInfo()->ifi_flags & IFF_UP) != 0);
    ix.link = ((m.ifInfo()->ifi_flags & IFF_LOWER_UP) != 0);

    ixs.push_back(ix);
  }
  
  close(response.fd);

  return ixs;
}

void Dcc::removeVlans(vector<size_t> vlans)
{
  LOG(INFO) << "removeVlans(...)";
  for(size_t v : vlans) { LOG(INFO) << "\t" << v; }

  aug_.load();

  auto bpo = bridgePath();
  if(!bpo) return;
  auto bp = *bpo;
  
  removePortsFromVlan(vlans, false);

  auto bridgeVids = aug_.get(bp+"/bridge-vids");
  vector<size_t> vs;
  if(bridgeVids) vs = parseVlist(*bridgeVids);
  for(size_t vlan : vlans)
  {
    removeVlan(vlan, vs);
  }
  if(!vs.empty())
  {
    auto value = emitVlist(vs);
    aug_.set(bp, "bridge-vids", value);
  }
  else
  {
    aug_.clear(bp, "bridge-vids");
  }

  aug_.save();

  ifup("bridge");
  
}

void Dcc::removePortsFromVlan(vector<size_t> vlans, bool load_save)
{
  LOG(INFO) << "removePortsFromVlan([...],"<<load_save<<")";
  if(load_save) aug_.load();

  vector<string> toCycle;  
  for(const auto v : vlans)
  {
    auto members = vlanMembers(v, false);
    for(const auto ifx : members)
    {
      removeBridgeVid(ifx, v);
      removeBridgeAccess(ifx, v);
      toCycle.push_back(ifx);
    }
  }

  aug_.save();

  for(const string & ifx : toCycle)
  {
    ifup(ifx);
  }
}


void Dcc::setPortVlan(vector<string> ifxs, size_t vlan)
{
  LOG(INFO) << "setPortVlan([...]," << vlan << ")";
  for(const string ifx : ifxs)
  {
    LOG(INFO) << "ifx=" << ifx;
  }

  for(const string ifx : ifxs)
  {
    if(isTrunk(ifx))
    {
      LOG(INFO) << ifx << " trunk("<<vlan<<")";
      setIfxVids(ifx, {vlan}, true);
    }
    else
    {
      LOG(INFO) << ifx << " access("<<vlan<<")";
      setBridgeAccess(ifx, vlan);
    }
    setIfxVids("bridge", {vlan}, true);
  }

  aug_.save();

  for(const string ifx : ifxs)
  {
    ifup(ifx);
  }
  ifup("bridge");
}

static bool isDownlink(string ifx)
{
 regex rx{"^swp[1-9][0-9]?s[0-3]$"};
 smatch s;
 return regex_match(ifx, s, rx);
}

void Dcc::delPortVlan(vector<string> ifxs, size_t vlan)
{
  LOG(INFO) << "delPortVlan([...]," << vlan << ")";

  for(const string & ifx : ifxs)
  {
    removeBridgeAccess(ifx, vlan);
    removeBridgeVid(ifx, vlan);
  }

  aug_.save();

  for(const string & ifx : ifxs)
  {
    ifup(ifx);
  }
}
      
void Dcc::removeSomePortsFromVlan(size_t vlan, vector<string> ifxs)
{
  LOG(INFO) << "removeSomePortsFromVlan(" << vlan << ",[...])";
  aug_.load();
  for(const string & ifx : ifxs)
  {
    removeBridgeVid(ifx, vlan);
    removeBridgeAccess(ifx, vlan);
  }
  aug_.save();
  for(const string & ifx : ifxs)
  {
    ifup(ifx);
  }
}
      
void Dcc::portControl(PortControlCommand cmd, vector<string> ifxs)
{
  LOG(INFO) << "portControl("<<cmd<<",[...])";
  using C = PortControlCommand;
  for(const string & ifx : ifxs)
  {
    switch(cmd)
    {
      case C::Enable: NetLink::enableIfx(ifx); break;
      case C::Disable: NetLink::disableIfx(ifx); break;
      case C::Speed100G: NetLink::setIfxSpeed(ifx, SPEED_1000000); break;
      case C::Speed40G: NetLink::setIfxSpeed(ifx, SPEED_40000); break;
      case C::Speed10G: NetLink::setIfxSpeed(ifx, SPEED_10000); break;
      case C::Speed1G: NetLink::setIfxSpeed(ifx, SPEED_1000); break;
      case C::Speed100M: NetLink::setIfxSpeed(ifx, SPEED_100); break;
      case C::Speed10M: NetLink::setIfxSpeed(ifx, SPEED_10); break;
      case C::DuplexFull: NetLink::setIfxDuplex(ifx, DUPLEX_FULL); break;
      case C::DuplexHalf: NetLink::setIfxDuplex(ifx, DUPLEX_HALF); break;
      case C::DuplexAuto: NetLink::setIfxDuplex(ifx, DUPLEX_UNKNOWN); break;
    }
  }
}

/*
 * Dcc -- Internals
 */

const std::string 
  Dcc::bridge_vids{"/bridge-vids"},
  Dcc::bridge_access{"/bridge-access"},
  Dcc::ifx_path{"/files/etc/network/interfaces"};

vector<string> Dcc::vlanMembers(size_t vid, bool doLoad)
{
  using namespace pipes;
  
  if(doLoad) { aug_.load(); }

  string path = fmt::format(
      "{}/*{}[ . =~ regexp('.*{}.*') ]", 
      ifx_path, 
      bridge_vids, 
      vid
  );
  auto trunk_members = aug_.match(path)
    | map([&](const string &s){ return erase(s, bridge_vids); })
    | collect([&](const auto &x){ return aug_.get(x); })
    | filter([](const string &x) { return x == "bridge"; });


  path = fmt::format(
      "{}/*{}[ . = '{}' ]",
      ifx_path,
      bridge_access,
      vid
  );
  
  auto access_members = aug_.match(path)
    | map([&](const string &s){ return erase(s, bridge_access); })
    | collect([&](const auto &x){ return aug_.get(x); });


  //append access members to trunk members for return value
  trunk_members.insert(
      trunk_members.end(), 
      access_members.begin(), 
      access_members.end()
  );

  return trunk_members;
};

vector<size_t> Dcc::parseVlist(string s)
{
  using namespace pipes;
  vector<string> vs = split(s, ' ');
  auto result = vs | map([](const string &x) -> size_t { return stoul(x); });
  return result;
}

void Dcc::addVlan(size_t v, vector<size_t> & vs)
{
  if(find(vs.begin(), vs.end(), v) == vs.end())
  {
    vs.push_back(v);
  }
}

void Dcc::removeVlan(size_t v, vector<size_t> & vs)
{
  auto i = find(vs.begin(), vs.end(), v);
  if(i != vs.end()) vs.erase(i);
}

optional<string> Dcc::bridgePath()
{
  auto path = aug_.match(fmt::format("{}/iface[ . = 'bridge' ]", ifx_path));
  if(path.empty()) return optional<string>{};
  else 
  {
    string s = path[0];
    return make_optional(s);
  }

  /*
  auto path = aug_.match(fmt::format("{}/iface[ . = 'bridge' ]", ifx_path));
  if(path.empty()) throw runtime_error{"could not find bridge interface"};
  return path[0];
  */
}

string Dcc::ifxPath(string ifx)
{
  auto path = aug_.match(fmt::format("{}/iface[ . = '{}' ]", ifx_path, ifx));
  if(path.empty()) throw runtime_error{"could not find interface " + ifx};
  return path[0];
}

string Dcc::emitVlist(vector<size_t> vs)
{
  string value;
  for(auto x : vs)
  {
    value += to_string(x) + " ";
  }

  //augeas wont save a value with a trailing space!
  value = value.substr(0, value.length() - 1);

  return value;
}

void Dcc::removeAccessPort(string ifx)
{
  aug_.clear(ifxPath(ifx), "bridge-access");
  aug_.save();
}

void Dcc::setBridgeAccess(string ifx, size_t vlan)
{
  auto path = ifxPath(ifx);
  aug_.set(path, "bridge-access", to_string(vlan));
  aug_.set(path, "bridge-allow-untagged", "yes");
}

void Dcc::addBridgeVid(string ifx, size_t vlan)
{
  auto path = ifxPath(ifx);
  auto ifx_vids = aug_.get(path+"/bridge-vids");
  if(!ifx_vids) return;
  
  auto vlist = parseVlist(*ifx_vids);
  addVlan(vlan, vlist);
  aug_.set(path, "bridge-vids", emitVlist(vlist));
}

void Dcc::removeBridgeAccess(string ifx, size_t vlan)
{
  auto path = ifxPath(ifx);
  auto ifx_access = aug_.get(path+"/bridge-access");
  if(!ifx_access) return;
  if(stoul(*ifx_access) == vlan) aug_.clear(path, "bridge-access");
}

void Dcc::removeBridgeVid(string ifx, size_t vlan)
{
  auto path = ifxPath(ifx);
  auto ifx_vids = aug_.get(path+"/bridge-vids");
  if(!ifx_vids) return;
  
  auto vlist = parseVlist(*ifx_vids);
  removeVlan(vlan, vlist);
  if(vlist.empty()) 
    aug_.clear(path, "bridge-vids");
  else 
    aug_.set(path, "bridge-vids", emitVlist(vlist));
}

bool Dcc::isTrunk(string ifx)
{
  auto path = ifxPath(ifx);
  auto allow_untagged = aug_.get(path+"/bridge-allow-untagged");
  if(allow_untagged)
  {
    return *allow_untagged == "no";
  }
  else
  {
    LOG(WARNING) << ifx << " is missing tagstate";
    return false;
  }
}

  ostream & deter::operator<<(ostream & o, const PortControlCommand & c)
  {
    using C = PortControlCommand;
    switch(c)
    {
      case C::Enable: o << "Enable"; break;
      case C::Disable: o << "Disable"; break;
      case C::Speed100G: o << "Speed100G"; break;
      case C::Speed40G: o << "Speed40G"; break;
      case C::Speed10G: o << "Speed10G"; break;
      case C::Speed1G: o << "Speed1G"; break;
      case C::Speed100M: o << "Speed100M"; break;
      case C::Speed10M: o << "Speed10M"; break;
      case C::DuplexFull: o << "DuplexFull"; break;
      case C::DuplexHalf: o << "DuplexHalf"; break;
      case C::DuplexAuto: o << "DuplexAuto"; break;
    }

    return o;
  }

  ///~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  /// State
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  void SwitchState::save()
  {
    ofstream ofs{"/tmp/dcc_state.json"};
  ofs << json().dump(2);
  ofs.close();
}

void SwitchState::load()
{
  ifstream ifs{"/tmp/dcc_state.json"};
  if(!ifs.good())
  {
    LOG(WARNING) << "no state file found";
    return; 
  }

  stringstream buf;
  buf << ifs.rdbuf();
  Json j = Json::parse(buf);
  *this = fromJson(j);
}

Json PortState::json() const
{
  Json j;
  j["trunked"] = trunked;
  return j;
}

PortState PortState::fromJson(Json j)
{
  PortState p;
  p.trunked = j.at("trunked");
  return p;
}

Json SwitchState::json() const
{
  Json j;
  
  j["ports"] = Json{};
  for(const auto &p : ports)
  {
     j["ports"][p.first] = p.second.json(); 
  }

  return j;
}

SwitchState SwitchState::fromJson(Json j)
{
  SwitchState s;

  Json ports = j.at("ports");
  for(Json::iterator it = ports.begin(); it != ports.end(); ++it)
  {
    s.ports[it.key()] = PortState::fromJson(it.value());
  }

  return s;
}
