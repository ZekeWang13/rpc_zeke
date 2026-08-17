#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include "checkOrder.pb.h"
#include "mprpcapplication.h"
#include "rpcprovider.h"

struct order
{
    std::string createTime;
    int price;
};


class OrderService : public fixbug::OrderServiceRPC{
public:
    std::unordered_map<std::string, order> orderlist;
    OrderService() = default;
    ~OrderService() = default;

    order check(std::string number){
        order resOrder = {};

        if (orderlist.find(number) == orderlist.end()) {
            return resOrder;
        }
        else {
            resOrder = {orderlist[number].createTime, orderlist[number].price};
        }

        return resOrder;
    }
    void check(::google::protobuf::RpcController* controller,
                       const ::fixbug::orderMessageRequest* request,
                       ::fixbug::orderMessageResponse* response,
                       ::google::protobuf::Closure* done){
        std::string number = request->ordernumber();
        order orderResponse = check(number);
        response->set_createtime(orderResponse.createTime);
        response->set_price(orderResponse.price);

        done->Run();
    }
};

int main(int argc, char **argv){
    OrderService os;
    order o1= {"20260328", 500};
    order o2= {"20260417", 150};
    os.orderlist["001"] = o1;
    os.orderlist["002"] = o2;

    MprpcApplication::Init(argc, argv);

    RpcProvider rp;
    rp.NotifyService(&os);
    rp.Run();
    
    return 0;
}