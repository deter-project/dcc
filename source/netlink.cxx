#include "netlink.hxx"
#include <stdexcept>
#include <bitset>
#include <linux/ethtool.h>
#include <iostream>
#include <glog/logging.h>
#include <fmt/format.h>

using namespace deter;
using std::vector;
using std::string;
using std::bitset;
using std::runtime_error;

int NetLink::testSock_{0};

int NetLink::testSock()
{
  if(testSock_ <= 0)
  {
    testSock_ = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if(testSock_ < 0) {
      LOG(ERROR) << "failed to get test socket";
      throw runtime_error{"fail to get test socket"};
    }
  }

  return testSock_;
}

NetLink::Request::Request()
{
  memset(&header, 0, sizeof(header));
  memset(&msg, 0, sizeof(msg));
  header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
  header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
  header.nlmsg_type = RTM_GETLINK;
  header.nlmsg_seq = 0;
  msg.ifi_family = AF_UNSPEC;
  msg.ifi_change = 0xffffffff;
}

int NetLink::tx(Request req)
{
  sockaddr_nl sa;
  iovec iov = {&req, req.header.nlmsg_len};
  msghdr msg = {&sa, sizeof(sa), &iov, 1, nullptr, 0, 0};
  memset(&sa, 0, sizeof(sa));
  sa.nl_family = AF_NETLINK;

  int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  sendmsg(fd, &msg, 0);

  return fd;
}

NetLink::Response NetLink::rx(int fd)
{
  size_t rxd{0};
  sockaddr_nl sa;

  char buf[16192];

  NetLink::Response rs;
  rs.fd = fd;
  bool over{false};
  while(!over)
  {
    memset(buf, 0, 16192);
    iovec iov = {buf, 16192};
    msghdr msg = {&sa, sizeof(sa), &iov, 1, nullptr, 0, 0};

    size_t len = recvmsg(fd, &msg, 0);

    rs.data = (char*)realloc(rs.data, rxd+len);
    memcpy(&rs.data[rxd], buf, len);
    char *bp = &rs.data[rxd];
    rxd += len;

    for(nlmsghdr *nh=(nlmsghdr*)bp; NLMSG_OK(nh, len); nh=NLMSG_NEXT(nh, len))
    {
      if(nh->nlmsg_type == NLMSG_DONE) 
      {
        over = true;
        break;
      }
    }
  }

  for(nlmsghdr *nh = (nlmsghdr*)rs.data; 
      NLMSG_OK(nh, rxd); 
      nh = NLMSG_NEXT(nh, rxd)
  )
  {
    NetLink::Response::Message m;
    m.header = nh;

    if(nh->nlmsg_type != RTM_BASE) continue;

    ifinfomsg *msg = (ifinfomsg*)NLMSG_DATA(nh);
    if(msg->ifi_type != ARPHRD_ETHER) continue;

    rtattr *rta = IFLA_RTA(msg);
    int alen = nh->nlmsg_len - NLMSG_LENGTH(sizeof(*msg));

    for(; RTA_OK(rta, alen); rta = RTA_NEXT(rta, alen)) 
      m.attributes.push_back(rta);

    rs.messages.push_back(m);
  }

  return rs;
}

ifinfomsg* NetLink::Response::Message::ifInfo() const
{
  return (ifinfomsg*)NLMSG_DATA(header);
}

NetLink::Response NetLink::getLink()
{
  return rx(tx());
}

NetLink::Response::~Response()
{
  free(data);
}

size_t NetLink::linkSpeed(string ifx)
{
  struct ifreq ifr;
  struct ethtool_cmd edata;
  
  memset(ifr.ifr_name, 0, sizeof(ifr.ifr_name));
  strncpy(ifr.ifr_name, ifx.c_str(), ifx.length());
  ifr.ifr_data = (char*)&edata;
  edata.cmd = ETHTOOL_GSET;

  int rc = ioctl(testSock(), SIOCETHTOOL, &ifr);
  if(rc < 0) {
    string msg = fmt::format("fail to ioctl ethtool for {}", ifx);
    LOG(ERROR) << msg;
    throw runtime_error{msg};
  }
  
  int spd = ethtool_cmd_speed(&edata);

  switch (spd) {
    case (__u32)SPEED_UNKNOWN: return 0;
    default: return spd;
  }
}

size_t NetLink::capSpeed(string ifx)
{
  ethtool_modinfo minfo;
  memset(&minfo, 0, sizeof(minfo));
  minfo.cmd = ETHTOOL_GMODULEINFO;

  ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strcpy(ifr.ifr_name, ifx.c_str());
  ifr.ifr_data = (char*)&minfo;


  int err = ioctl(testSock(), SIOCETHTOOL, &ifr);

  //this is not a module device
  if(err < 0) 
  {
    LOG(ERROR) << "err: SIOCETHTOOL " << err;
    return 0; 
  }


  ethtool_eeprom* einfo = 
    (ethtool_eeprom*)malloc(sizeof(ethtool_eeprom) + minfo.eeprom_len);
  memset(einfo, 0, sizeof(ethtool_eeprom) + minfo.eeprom_len);
  einfo->cmd = ETHTOOL_GMODULEEEPROM;
  einfo->len = minfo.eeprom_len;
  ifr.ifr_data = (char*)einfo;
  
  err = ioctl(testSock(), SIOCETHTOOL, &ifr);
  if(err < 0) throw runtime_error{"ioctl::ETHTOOL_GMODULEEEPROM failed"};

  unsigned char* eeprom_data = (unsigned char*)einfo->data;

  unsigned short module_id = (unsigned short)eeprom_data[128];

  if(module_id == 0x0d)
  {
    LOG(INFO) << ifx << ": qsfp+ module detected";
  }
  else if(module_id == 0x11)
  {
    LOG(INFO) << ifx << ": qsfp28 module detected";
  }
  else if(module_id == 0)
  {
    LOG(WARNING) << ifx << ": module not detected";
    return 0;
  }
  else
  {
    LOG(WARNING) << ifx << ": unknown module id " << std::hex << module_id;
    return 0;
  }

  auto tcode = std::bitset<8>(eeprom_data[131]);
  if(tcode.test(0))
  {
    LOG(INFO) << ifx << ": 40G Active transceiver detected";
    return 40000;
  }
  
  if(tcode.test(1))
  {
    LOG(INFO) << ifx << ": 40G LR4 transceiver detected";
    return 40000;
  }
  
  if(tcode.test(2))
  {
    LOG(INFO) << ifx << ": 40G SR4 transceiver detected";
    return 40000;
  }
  
  if(tcode.test(3))
  {
    LOG(INFO) << ifx << ": 40G CR4 transceiver detected";
    return 40000;
  }

  if(tcode.test(4))
  {
    LOG(INFO) << ifx << ": 10G SR transceiver detected";
    return 10000;
  }

  if(tcode.test(5))
  {
    LOG(INFO) << ifx << ": 10G LR transceiver detected";
    return 10000;
  }

  if(tcode.test(6))
  {
    LOG(INFO) << ifx << ": 10G LRM transceiver detected";
    return 10000;
  }

  return 0;
}

size_t NetLink::ifxIndex(string ifx)
{
  struct ifreq ifr;
  memset(ifr.ifr_name, 0, sizeof(ifr.ifr_name));
  strncpy(ifr.ifr_name, ifx.c_str(), ifx.length());
  int err = ioctl(testSock(), SIOCGIFINDEX, &ifr);
  if(err == -1)
    throw runtime_error{"fail to get ifx index for "+ifx};

  return ifr.ifr_ifindex;
}

//TODO check netlink response? fire and forget for now
void NetLink::enableIfx(string ifx)
{
  Request rq;
  rq.header.nlmsg_type = RTM_SETLINK;
  rq.msg.ifi_index = ifxIndex(ifx);
  rq.msg.ifi_flags |= IFF_UP;
  tx(rq);
}

//TODO check netlink response? fire and forget for now
void NetLink::disableIfx(string ifx)
{
  Request rq;
  rq.header.nlmsg_type = RTM_SETLINK;
  rq.msg.ifi_index = ifxIndex(ifx);
  rq.msg.ifi_flags &= ~(IFF_UP | IFF_LOWER_UP);
  tx(rq);
}

//TODO this really does nothing at the end of the day, cumulus does not have 
//support or setting the link speed except for in breakout port scenarios
void NetLink::setIfxSpeed(string ifx, uint32_t speed)
{
  //std::cout << "setIfxSpeed - " << ifx << std::endl;

  struct ifreq ifr;
  struct ethtool_cmd edata;
  
  memset(ifr.ifr_name, 0, sizeof(ifr.ifr_name));
  strncpy(ifr.ifr_name, ifx.c_str(), ifx.length());
  ifr.ifr_data = (char*)&edata;
  edata.cmd = ETHTOOL_GSET;

  int rc = ioctl(testSock(), SIOCETHTOOL, &ifr);
  if(rc < 0) {
    LOG(ERROR) << "setIfxSpeed:: fail to ioctl ethtool::sset for " 
               << ifx;
    return;
  }

  ethtool_cmd_speed_set(&edata, speed);
  edata.speed = speed;
  edata.cmd = ETHTOOL_SSET;
  ifr.ifr_data = (char*)&edata;
  rc = ioctl(testSock(), SIOCETHTOOL, &ifr);
  if(rc < 0) {
    LOG(ERROR) << "setIfxSpeed:: fail to ioctl ethtool::sset for " 
               << ifx;
    return;
  }
}

void NetLink::setIfxDuplex(string ifx, int duplex)
{
  //std::cout << "setIfxDuplex - " << ifx << std::endl;

  struct ifreq ifr;
  struct ethtool_cmd edata;
  
  memset(ifr.ifr_name, 0, sizeof(ifr.ifr_name));
  strncpy(ifr.ifr_name, ifx.c_str(), ifx.length());
  ifr.ifr_data = (char*)&edata;
  edata.cmd = ETHTOOL_GSET;

  int rc = ioctl(testSock(), SIOCETHTOOL, &ifr);
  if(rc < 0) {
    LOG(ERROR) << "setIfxDuplex:: fail to ioctl ethtool::sset for " 
               << ifx;
    return;
  }

  edata.cmd = ETHTOOL_SSET;
  edata.duplex = duplex;
  ioctl(testSock(), SIOCETHTOOL, &ifr);
}
