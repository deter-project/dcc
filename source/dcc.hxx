#pragma once

#include <vector>
#include <set>
#include <string>
#include <experimental/optional>
#include <unordered_map>
#include <mutex>
#include "augeas.hxx"
#include "json.hxx"

namespace deter
{
  using Json = nlohmann::json;

  class Dcc;
  struct VlanInfo;
  struct Interface;

  enum class PortControlCommand : int {
    Enable,
    Disable,
    Speed100G,
    Speed40G,
    Speed10G,
    Speed1G,
    Speed100M,
    Speed10M,
    DuplexFull,
    DuplexHalf,
    DuplexAuto
  };

  std::ostream & operator<<(std::ostream & o, const PortControlCommand &);


  struct PortState
  {
    bool trunked;

    Json json() const;
    static PortState fromJson(Json j);
  };
  
  struct SwitchState
  {
    std::unordered_map<std::string, PortState> ports;

    void save();
    void load();
    Json json() const;
    static SwitchState fromJson(Json j);
  };

  class Dcc
  {
    public:
      std::vector<VlanInfo> listVlans();

      std::vector<std::pair<size_t, std::experimental::optional<size_t>>> 
      findVlans(std::vector<size_t> ids = {});

      bool vlanHasPorts(size_t vlan_id);
      
      std::vector<Interface> getInterfaces();

      void disablePortTrunking(std::string ifx, bool finalize = true);
      bool enablePortTrunking(std::string ifx, size_t vlan_id, bool eq_trunk);
      void setVlansOnTrunk(std::string ifx, std::vector<size_t> vlans,
          bool allow);
      void removeVlans(std::vector<size_t> vlans);
      void delPortVlan(std::vector<std::string> ifxs, size_t vlan);
      void setPortVlan(std::vector<std::string> ifx, size_t vlan);
      void removePortsFromVlan(std::vector<size_t> vlans, bool load_save = true);
      void removeSomePortsFromVlan(size_t vlan, std::vector<std::string> ifxs);
      void portControl(PortControlCommand cmd, std::vector<std::string> ifxs);

    private:
      std::vector<std::string> vlanMembers(size_t vid, bool doLoad = true);
      std::string emitVlist(std::vector<size_t> vids);
      std::vector<size_t> parseVlist(std::string);
      void addVlan(size_t v, std::vector<size_t> & vs);
      void removeVlan(size_t v, std::vector<size_t> & vs);
      std::experimental::optional<std::string> bridgePath();
      std::string ifxPath(std::string ifx);
      //void setAccessPort(std::string ifx, size_t vlan);
      void removeAccessPort(std::string ifx);
      void setIfxVids(std::string ifx, std::vector<size_t> vlans, bool allow);

      void setBridgeAccess(std::string ifx, size_t vlan);
      void addBridgeVid(std::string ifx, size_t vlan);
      void removeBridgeAccess(std::string ifx, size_t vlan);
      void removeBridgeVid(std::string ifx, size_t vlan);

      bool isTrunk(std::string ifx);
      void updateActiveInterfaces();
      
      //interfaces that have an entry in /etc/network/interfaces
      std::set<std::string> activeIfxs_;
      Augeas aug_;

      SwitchState state_;
      static const std::string 
        bridge_access,
        bridge_vids,
        ifx_path;
  };

  struct VlanInfo
  {
    VlanInfo() = default;
    VlanInfo(size_t id);
    VlanInfo(size_t id, std::vector<std::string>);
    VlanInfo(size_t deterId, size_t cumulusId, std::vector<std::string>);
    
    size_t deterId{0}, cumulusId{0};
    std::vector<std::string> members;
  };

  struct Interface
  {
    std::string 
      name,  
      mac,
      duplex{"full"};

    bool enabled{false}, link{false};

    size_t 
      capSpeed{0},
      linkSpeed{0};
  };

}
