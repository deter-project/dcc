/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  Deter Cumulus Controller
 *  ------------------------
 *
 *  This is the main application code that controls a Cumulus Linux switch. It
 *  the API is designed to be called by the cumulus snmpit module 
 *  (/tbsetup/snmpit_cumuls.pm)
 *
 *  Copyright The Deter Project (c) 2016. All rights reserved.
 *  License: LGPL
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

#include <sys/prctl.h>
#include <iostream>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <functional>
#include <thread>
#include <mhttpdxx/microhttpd.hxx>
#include <experimental/optional>
#include <glog/logging.h>
#include <fmt/format.h>
#include <gflags/gflags.h>
#include <map>
#include <mutex>
#include "dcc.hxx"
#include "pipes.hxx"

using std::experimental::optional;
using std::experimental::make_optional;
using std::vector;
using std::unique_ptr;
using std::to_string;
using std::ofstream;
using std::find_if;
using std::string;
using std::function;
using std::exception;
using std::mutex;
using std::lock_guard;
using std::thread;
using namespace deter;
using namespace httpd;
//using Json = nlohmann::json;

//static globals
Dcc dcc;

//api level functions
void ding();
void listVlans();
void findVlans();
void vlanHasPorts();
void listPorts();
void disablePortTrunking();
void enablePortTrunking();
void setVlansOnTrunk();
void removeVlans();
void setPortVlan();
void delPortVlan();
void removePortsFromVlan();
void removeSomePortsFromVlan();
void portControl();
void createVlan();


Server &srv = Server::get();
static mutex mtx{};


//TODO: perform finer grained locking in dcc itself later

static void safePost(string path, function<Response(PostRequest)> handler)
{
  auto safe_handler = [handler, path](PostRequest m)
  {
    try
    { 
      lock_guard<mutex> lk{mtx};
      return handler(m); 
    }
    catch(exception &e)
    {
      LOG(ERROR) << path << " exception:" << e.what();
      Json r;
      r["result"] = "exception";
      r["info"] = e.what();
      return Response{ Status::ServerError, r.dump(2) };
    }
  };
  srv.onPost(path, safe_handler);
}

static void safeGet(string path, function<Response(GetRequest)> handler)
{
  auto safe_handler = [handler, path](GetRequest m)
  {
    try
    { 
      lock_guard<mutex> lk{mtx};
      return handler(m); 
    }
    catch(exception &e)
    {
      LOG(ERROR) << path << " exception:" << e.what();
      Json r;
      r["result"] = "exception";
      r["info"] = e.what();
      return Response{ Status::ServerError, r.dump(2) };
    }
  };
  srv.onGet(path, safe_handler);
}

std::map<size_t, string> vmap;

static void saveVmap()
{
  std::vector<Json> j;
  for(auto p : vmap) j.push_back(Json::array({p.first, p.second}));
  std::ofstream ofs{"/tmp/vmap.json"};
  Json jj = j;
  ofs << jj.dump(2);
  ofs.close();
}

static optional<size_t> vlanNumber(string vid)
{
  auto x = 
  find_if(vmap.begin(), vmap.end(), [vid](auto p) 
  {
      return p.second == vid;
  });

  if(x != vmap.end()) { return make_optional(x->first); }
  else return optional<size_t>{};
}

static void loadVmap()
{
  std::ifstream ifs{"/tmp/vmap.json"};
  if(!ifs.good()) 
  {
    LOG(WARNING) << "no vlan state file found";
    return;
  }

  std::stringstream buf;
  buf << ifs.rdbuf();
  try 
  { 
    Json j = Json::parse(buf); 
    for(auto p : j) vmap[p[0]] = p[1];
  }
  catch(...)
  {
    LOG(WARNING) << "invalid vlan state file found - it will be overwritten";
    return;
  }
}

int main(int argc, char **argv)
{
  google::SetUsageMessage("usage: materialization");
  google::ParseCommandLineFlags(&argc, &argv, true);

  google::InitGoogleLogging("dcc");
  google::InstallFailureSignalHandler();

  prctl(PR_SET_DUMPABLE, 1); 
  LOG(INFO) << "dcc starting";

  loadVmap();

  //handlers
  ding();
  listVlans();
  findVlans();
  vlanHasPorts();
  listPorts();
  disablePortTrunking();
  enablePortTrunking();
  setVlansOnTrunk();
  removeVlans();
  setPortVlan();
  delPortVlan();
  removePortsFromVlan();
  removeSomePortsFromVlan();
  portControl();
  createVlan();

  //go
  srv.run();
}

/* -----------------------------------------------------------------------------
 * ding
 * -----
 *  
 *  response:
 *    dong: text - text that is literally "dong"
 */

void ding()
{
  safeGet("/ding", [](GetRequest) {

    return Response{ Status::OK, "dong" };

  });
}

/* -----------------------------------------------------------------------------
 * createVlan
 * ----------
 *  
 *  response:
 *    vlan_number: the number of the created vlan
 */

void createVlan()
{
  safePost("/createVlan", [](PostRequest m) {

    Json request = Json::parse(m.data);

    string vid = request.at("vlan_id");
    size_t vnumber = request.at("vlan_number");

    auto x = vmap.find(vnumber);
    if (x != vmap.end())
    {
      vnumber = vmap.rbegin()->first + 1; //remember this is an _ordered_ map
    }

    vmap[vnumber] = vid;
    saveVmap();

    Json result;
    result["vlan_number"] = vnumber;

    return Response{ Status::OK, result.dump(2) };

  });
}


/* -----------------------------------------------------------------------------
 * listVlans
 * ---------
 *  
 *  response:
 *    vlans: json - a json object that is a list of VlanInfo objects
 */

void listVlans()
{
  safeGet("/listVlans", [](GetRequest) {

    using namespace pipes;

    Json j =
      dcc.listVlans()
      | map([](const auto &i){
          return Json::array({vmap[i.deterId], i.cumulusId, i.members});
        });

    return Response{ Status::OK, j.dump(2) };

  });
}

/* -----------------------------------------------------------------------------
 * findVlans
 * ---------
 *  
 *  response:
 *    vlans: json - a json object that is a map from vlan ids to vlan numbers
 */
void findVlans()
{
  safePost("/findVlans", [](PostRequest m) {

    //vector<size_t> vlans = m.bodyAsJson();
    vector<string> vlans = Json::parse(m.data);

    vector<Json> r;

    if(vlans.empty())
    {
      for(auto p : vmap)
      {
        r.push_back(Json::array({p.first, p.second}));
      }
    }
    else
    {
      for(string vid : vlans)
      {
        auto vn = vlanNumber(vid);
        if(vn) r.push_back(Json::array({*vn, vid}));
        else   r.push_back(Json::array({nullptr, vid}));
      }
    }

    Json j = r;

    /*
    Json j = 
      dcc.findVlans(vlans)
      | map([](const auto &p)
        {
          if(p.second) return Json::array({p.first, *p.second});
          else         return Json::array({p.first, nullptr});
        });
    */

    return Response{ Status::OK, j.dump(2) };

  });
}

/* -----------------------------------------------------------------------------
 * vlanHasPorts
 * -------------
 *
 *  parameters:
 *    - { id: <id> }
 *    
 *  
 *  response:
 *      - {exists: true} if the supplied vlan id has ports
 *      - {exists: false} if the supplied vlan id has no ports
 */
void vlanHasPorts()
{
  safePost("/vlanHasPorts", [](PostRequest m) {

      Json request = Json::parse(m.data);
      size_t vlanId = request.at("id");

      Json result; 
      result["exists"] = dcc.vlanHasPorts(vlanId);

      return Response{ Status::OK, result.dump(2) };

  });
}

/* -----------------------------------------------------------------------------
 * listPorts
 * ---------
 *
 *  response:
 *    ports: json - a json object that is a list of PortInfo objects
 */

void listPorts()
{
  safeGet("/listPorts", [](GetRequest) {

      using namespace pipes;

      Json j = 
        dcc.getInterfaces()
        | filter([](const auto &i) { return i.name == "eth0"; })
        | map([](const auto &i){

            return Json::array({
                i.name, 
                i.enabled ? "yes" : "no", 
                i.link ? "up" : "down", 
                to_string(i.linkSpeed) + "Mbps", 
                i.duplex
            });

          });

     return Response{ Status::OK, j.dump(2) };
  });
}

/* -----------------------------------------------------------------------------
 * disablePortTrunking
 * -------------------
 *
 *  parameters:
 *    - { port: <port name> }
 *
 *  response:
 *    { "result": "ok" }
 */

void disablePortTrunking()
{
  safePost("/disablePortTrunking", [](PostRequest m) {

      Json request = Json::parse(m.data);
      string ifx = request.at("port");

      Json result;
      result["result"] = "ok";

      dcc.disablePortTrunking(ifx);

      return Response{ Status::OK, result.dump(2) };
  });
}

/* -----------------------------------------------------------------------------
 * enablePortTrunking
 * ------------------
 *
 *  parameters:
 *    - { 
 *        port: <port name>,
 *        vlan: <vlan id>,
 *        eqtrunk: <equal trunking>
 *      }
 *
 *  response:
 *    { "result": "ok" }
 */

void enablePortTrunking()
{
  safePost("/enablePortTrunking", [](PostRequest m) {

      Json request = Json::parse(m.data);

      string ifx = request.at("port");
      size_t vlan = request.at("vlan");

      bool eqtrunk = false;

      Json result;

      bool r = dcc.enablePortTrunking(ifx, vlan, eqtrunk);
      r ? result["result"] = "ok" : result["result"] = "fail";

      return Response{ Status::OK, result.dump(2) };

  });
}

/* -----------------------------------------------------------------------------
 * setVlansOnTrunk
 * ---------------
 *
 *  parameters:
 *    - { 
 *        port: <port name>,
 *        vlans: [<vlan id>],
 *        allow: bool
 *      }
 *
 *  response:
 *    { "result": "ok" }
 */

void setVlansOnTrunk()
{
  safePost("/setVlansOnTrunk", [](PostRequest m) {

    Json request = Json::parse(m.data);

    string ifx = request.at("port");
    vector<size_t> vlans = request.at("vlans");
    bool allow = request.at("allow");

    Json result;
    result["result"] = "ok";

    dcc.setVlansOnTrunk(ifx, vlans, allow);

    return Response{ Status::OK, result.dump(2) };

  });
}

/* -----------------------------------------------------------------------------
 * removeVlans
 * -----------
 *
 *  parameters:
 *    - { 
 *        [vlan]: <vlan ids>,
 *      }
 *
 *  response:
 *    { "result": "ok" }
 */
void removeVlans()
{
  safePost("/removeVlans", [](PostRequest m) {

      Json request = Json::parse(m.data);
      vector<size_t> vlans = request.at("vlan");
      Json result;
      result["result"] = "ok";
      dcc.removeVlans(vlans);
      for(size_t v : vlans) { vmap.erase(v); }
      saveVmap();
      return Response{ Status::OK, result.dump(2) };

  });
}

void setPortVlan()
{
  safePost("/setPortVlan", [](PostRequest m) {
    
      Json request = Json::parse(m.data);
      vector<string> ifxs = request.at("ports");
      size_t vlan = request.at("vlan");
      dcc.setPortVlan(ifxs, vlan); 

      Json result;
      result["result"] = "ok";
      return Response{ Status::OK, result.dump(2) };
  });
}

void delPortVlan()
{

  safePost("/delPortVlan", [](PostRequest m) {

      Json request = Json::parse(m.data);
      vector<string> ifxs = request.at("ports");
      size_t vlan = request.at("vlan");

      Json result;
      result["result"] = "ok";
      dcc.delPortVlan(ifxs, vlan);
      return Response{ Status::OK, result.dump(2) };
  });

}

void removePortsFromVlan()
{
  safePost("/removePortsFromVlan", [](PostRequest m) {

      Json request = Json::parse(m.data);
      vector<size_t> vlans = request.at("vlans");
      
      Json result;
      result["result"] = "ok";
      dcc.removePortsFromVlan(vlans);
      return Response{ Status::OK, result.dump(2) };
  });
}

void removeSomePortsFromVlan()
{
  safePost("/removeSomePortsFromVlan", [](PostRequest m) {

      Json request = Json::parse(m.data);
      size_t vlan = request.at("vlan");
      vector<string> ifxs = request.at("ports");
      
      Json result;
      result["result"] = "ok";
      dcc.removeSomePortsFromVlan(vlan, ifxs);
      return Response{ Status::OK, result.dump(2) };
  });
}

void portControl()
{
  safePost("/portControl", [](PostRequest m) {

      Json request = Json::parse(m.data);
      string command = request.at("command");
      vector<string> ifxs = request.at("ports");

      using Cmd = PortControlCommand;
      
      Json result;
      result["result"] = "ok";
      
      PortControlCommand cmd;
      if(command == "enable") cmd = Cmd::Enable;
      else if(command == "disable") cmd = Cmd::Disable;
      else if(command == "100000mbit") cmd = Cmd::Speed100G;
      else if(command == "40000mbit") cmd = Cmd::Speed40G;
      else if(command == "10000mbit") cmd = Cmd::Speed10G;
      else if(command == "1000mbit") cmd = Cmd::Speed1G;
      else if(command == "100mbit") cmd = Cmd::Speed100M;
      else if(command == "10mbit") cmd = Cmd::Speed10M;
      else if(command == "full") cmd = Cmd::DuplexFull;
      else if(command == "half") cmd = Cmd::DuplexHalf;
      else if(command == "auto") cmd = Cmd::DuplexAuto;
      else
      {
        result["result"] = "fail";
        result["info"] = "unknown command `"+command+"`";
        return Response{ Status::OK, result.dump(2) };
      }
      
      dcc.portControl(cmd, ifxs);

      return Response{ Status::OK, result.dump(2) };
  });
}
