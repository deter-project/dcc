#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <linux/if.h>
#include <net/if_arp.h>
//#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <iostream>

//Need these for some old-ish ethtool versions
#ifndef SPEED_1000000
#define SPEED_1000000 1000000
#endif

#ifndef SPEED_40000
#define SPEED_40000 40000
#endif

namespace deter
{

  template <typename T>
  T getAttr(const rtattr *a);
  
  template <>
  inline
  std::string getAttr<std::string>(const rtattr *a)
  {
    return std::string((char*)RTA_DATA(a));
  }

  struct NetLink
  {
    struct Request
    {
      Request();
      nlmsghdr header;
      ifinfomsg msg;
    };

    struct Response
    {
      virtual ~Response();
      struct Message
      {
        nlmsghdr* header;
        std::vector<rtattr*> attributes;

        template <typename T> 
        T getAttribute(int type) const
        {
          for(const auto a : attributes)
          {
            if(a->rta_type == type) return getAttr<T>(a); 
          }
          return T{};
        }

        ifinfomsg* ifInfo() const;
      };

      std::vector<Message> messages;
      char *data{nullptr};
      int fd{0};
    };

    static int tx(Request r = Request{});
    static Response rx(int);
    static Response getLink();

    static int testSock();

    //getters
    static size_t linkSpeed(std::string ifx);
    static size_t capSpeed(std::string ifx);
    static size_t ifxIndex(std::string ifx);

    //setters
    static void enableIfx(std::string ifx);
    static void disableIfx(std::string ifx);
    static void setIfxSpeed(std::string ifx, uint32_t speed);
    static void setIfxDuplex(std::string ifx, int duplex);
    
    private: 
    static int testSock_;

  };


}
