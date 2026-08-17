#include <iostream>
#include <string>
#include "mprpcapplication.h"
#include "checkOrder.pb.h"
#include "servicediscovery.h"

int main(int argc, char** argv){
    MprpcApplication::Init(argc, argv);
    ServiceDiscovery::GetInstance().Start();

    fixbug::OrderServiceRPC_Stub stub(new MprpcChannel());
    while (true)
    {
        fixbug::orderMessageRequest request;
        request.set_ordernumber("001");

        fixbug::orderMessageResponse response;
        MprpcController controller;

        stub.check(&controller, &request, &response, nullptr);

        if (controller.Failed()) {
            std::cout << "rpc failed: " << controller.ErrorText() << std::endl;
        } else {
            std::cout << response.createtime() << " " << response.price() << std::endl;
        }

        sleep(2);
    }
    // 改为长调用
    // std::string requestr = "001";
    // ::fixbug::orderMessageRequest request;
    // request.set_ordernumber(requestr);
    // ::fixbug::orderMessageResponse response;
    
    // MprpcController controller;
    // stub.check(&controller, &request, &response, nullptr);

    // if (controller.Failed()) {
    //     std::cout << "rpc failed: " << controller.ErrorText() << std::endl;
    //     return 0;
    // }

    // std::cout << response.createtime() << std::endl;
    // std::cout << response.price() << std::endl;

    return 0;
}