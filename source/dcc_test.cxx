#include "catch.hpp"
#include "dcc.hxx"
#include "pipes.hxx"
#include <httpxx/json.hxx>
#include <iostream>

using namespace deter;
using Json = nlohmann::json;
using std::to_string;

/*
TEST_CASE("list vlans", "[dcc]")
{
  Dcc dcc;
  auto vlans = dcc.listVlans();
  REQUIRE( vlans.size() == 4 );
  REQUIRE( vlans[0].deterId == 100 );
  REQUIRE( vlans[0].cumulusId == 100 );
  REQUIRE( vlans[0].members.size() == 2);
  REQUIRE( vlans[0].members[0] == "swp1" );
  REQUIRE( vlans[0].members[1] == "swp4" );
}

TEST_CASE("find vlans", "[dcc]")
{
  Dcc dcc;
  auto vmap = dcc.findVlans();
  REQUIRE( vmap.size() == 4 );
  //REQUIRE( vmap.at(100) == 100 );
}

TEST_CASE("vlan has ports", "[dcc]")
{
  Dcc dcc;
  REQUIRE( dcc.vlanHasPorts(100) == true );
  REQUIRE( dcc.vlanHasPorts(700) == false );
}
*/

TEST_CASE("list interfaces", "[dcc]")
{
  using namespace pipes;
  Dcc dcc;
  /*
  std::cout << "getting interfaces" << std::endl;
  auto ixs = dcc.getInterfaces();
  for(auto ix : ixs)
  {
    std::cout << ix.name << std::endl
         << "  cap: " << ix.capSpeed << std::endl 
         << "  link: " << ix.linkSpeed << std::endl;
  }
  */
  Json j = 
    dcc.getInterfaces()
    | map([](const auto &i){

        return Json::array({
            i.name, 
            i.enabled ? "yes" : "no", 
            i.link ? "up" : "down", 
            to_string(i.capSpeed) + "Mbps", 
            i.duplex
        });

      });

  for(const auto x : j) std::cout << x.dump(2) << std::endl;
}
